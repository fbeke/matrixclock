#include "ds18x20.h"

#include "mtimer.h"

#include <util/delay.h>
#include <util/crc16.h>

static ds18x20Dev devs[DS18X20_MAX_DEV];
static uint8_t devCount = 0;

static uint8_t isResult = 0;							/* It conversion has been done */

static uint8_t calcCRC8 (uint8_t *arr, uint8_t arr_size)
{
	uint8_t i, crc;

	for (i = 0, crc = 0; i < arr_size; i++)
		crc = _crc_ibutton_update(crc, arr[i]);

	return crc;
}

static uint8_t ds18x20IsOnBus(void)
{
	uint8_t ret;

	DDR(ONE_WIRE) |= ONE_WIRE_LINE;						/* Pin as output (0) */
	PORT(ONE_WIRE) &= ~ONE_WIRE_LINE;					/* Set active 0 */
	_delay_us(500);										/* Reset */
	DDR(ONE_WIRE) &= ~ONE_WIRE_LINE;					/* Pin as input (1) */
	PORT(ONE_WIRE) |= ONE_WIRE_LINE;					/* Enable pull-up resitor */
	_delay_us(60);										/* Wait for response */

	ret = !(PIN(ONE_WIRE) & ONE_WIRE_LINE);

	_delay_us(440);

	return ret;
}

static void ds18x20SendBit(uint8_t bit)
{
	DDR(ONE_WIRE) |= ONE_WIRE_LINE;						/* Pin as output (0) */
	PORT(ONE_WIRE) &= ~ONE_WIRE_LINE;					/* Set active 0 */
	_delay_us(5);
	if (!bit)
		_delay_us(60);
	DDR(ONE_WIRE) &= ~ONE_WIRE_LINE;					/* Pin as input (1) */
	PORT(ONE_WIRE) |= ONE_WIRE_LINE;					/* Enable pull-up resitor */
	_delay_us(5);
	if (bit)
		_delay_us(60);

	return;
}

static uint8_t ds18x20GetBit(void)
{
	uint8_t ret;

	DDR(ONE_WIRE) |= ONE_WIRE_LINE;						/* Pin as output (0) */
	PORT(ONE_WIRE) &= ~ONE_WIRE_LINE;					/* Set active 0 */
	_delay_us(5);										/* Strob */
	DDR(ONE_WIRE) &= ~ONE_WIRE_LINE;					/* Pin as input (1) */
	PORT(ONE_WIRE) |= ONE_WIRE_LINE;					/* Enable pull-up resitor */
	_delay_us(5);

	ret = PIN(ONE_WIRE) & ONE_WIRE_LINE;

	_delay_us(60);

	return ret;
}

static void ds18x20SendByte(uint8_t byte)
{
	uint8_t i;

	for (i = 0; i < 8; i++)
		ds18x20SendBit(byte & (1<<i));

	return;
}

static uint8_t ds18x20GetByte(void)
{
	uint8_t i, ret = 0;

	for (i = 0; i < 8; i++)
		if (ds18x20GetBit())
			ret |= (1<<i);
		else
			ret &= ~(1<<i);

	return ret;
}

static void ds18x20Select(ds18x20Dev *dev)
{
	uint8_t i;

	ds18x20SendByte(DS18X20_CMD_MATCH_ROM);

	for (i = 0; i < 8; i++)
		ds18x20SendByte(dev->id[i]);

	return;
}

static void getAllTemps()
{
	uint8_t i, j;

	uint8_t arr[9];

	for (i = 0; i < devCount; i++) {
		if (ds18x20IsOnBus()) {
			ds18x20Select(&devs[i]);
			ds18x20SendByte(DS18X20_CMD_READ_SCRATCH);

			for (j = 0; j < 9; j++)
				arr[j] = ds18x20GetByte();

			if (!calcCRC8(arr, sizeof(arr))) {
				for (j = 0; j < 9; j++)
					devs[i].sp[j] = arr[j];
			}
		}
	}

	return;
}

static void convertTemp(void)
{
	ds18x20SendByte(DS18X20_CMD_SKIP_ROM);
	ds18x20SendByte(DS18X20_CMD_CONVERT);

#ifdef DS18X20_PARASITE_POWER
	/* Set active 1 on port for 750ms as parasitic power */
	PORT(ONE_WIRE) |= ONE_WIRE_LINE;
	DDR(ONE_WIRE) |= ONE_WIRE_LINE;
#endif

	setTempConvertTimer(TEMP_MEASURE_TIME);

	isResult = 1;

	return;
}

static uint8_t ds18x20SearchRom(uint8_t *bitPattern, uint8_t lastDeviation)
{
	uint8_t currBit;
	uint8_t newDeviation = 0;
	uint8_t bitMask = 0x01;
	uint8_t bitA;
	uint8_t bitB;

	/* Send SEARCH ROM command on the bus */
	ds18x20SendByte(DS18X20_CMD_SEARCH_ROM);

	/* Walk through all 64 bits */
	for (currBit = 0; currBit < 64; currBit++)
	{
		/* Read bit from bus twice. */
		bitA = ds18x20GetBit();
		bitB = ds18x20GetBit();

		if (bitA && bitB) {								/* Both bits 1 = ERROR */
			return 0xFF;
		} else if (!(bitA || bitB)) {					/* Both bits 0 */
			if (currBit == lastDeviation) {				/* Select 1 if device has been selected */
				*bitPattern |= bitMask;
			} else if (currBit > lastDeviation) {		/* Select 0 if no, and remember device */
				(*bitPattern) &= ~bitMask;
				newDeviation = currBit;
			} else if (!(*bitPattern & bitMask)) {		 /* Otherwise just remember device */
				newDeviation = currBit;
			}
		} else { /* Bits differ */
			if (bitA)
				*bitPattern |= bitMask;
			else
				*bitPattern &= ~bitMask;
		}

		/* Send the selected bit to the bus. */
		ds18x20SendBit(*bitPattern & bitMask);

		/* Adjust bitMask and bitPattern pointer. */
		bitMask <<= 1;
		if (!bitMask)
		{
			bitMask = 0x01;
			bitPattern++;
		}
	}

	return newDeviation;
}

void ds18x20SearchDevices(void)
{
	uint8_t i, j;
	uint8_t *newID;
	uint8_t *currentID;
	uint8_t lastDeviation;
	uint8_t count = 0;

	/* Reset addresses */
	for (i = 0; i < DS18X20_MAX_DEV; i++)
		for (j = 0; j < 8; j++)
			devs[i].id[j] = 0x00;

	/* Search all sensors */
	newID = devs[0].id;
	lastDeviation = 0;
	currentID = newID;

	do {
		for (j = 0; j < 8; j++)
			newID[j] = currentID[j];

		if (!ds18x20IsOnBus()) {
			devCount = 0;

			return;
		}

		lastDeviation = ds18x20SearchRom(newID, lastDeviation);

		currentID = newID;
		count++;
		newID=devs[count].id;

	} while (lastDeviation != 0);

	devCount = count;

	return;
}

uint8_t ds18x20Process(void)
{
	if (getTempConvertTimer() == 0) {

		if (isResult)
			getAllTemps();

		/* Convert temperature */
		if (ds18x20IsOnBus())
			convertTemp();
	}

	return devCount;
}

int16_t ds18x20GetTemp(uint8_t num)
{
	int16_t ret = 0;

	if (devs[num].id[0] == 0x28) /* DS18B20 */
		ret = (devs[num].sp[0] | (devs[num].sp[1] << 8)) * 5 / 8;
	else if (devs[num].id[0] == 0x10) /* DS18S20 */
		ret = (devs[num].sp[0] | (devs[num].sp[1] << 8)) * 2;

	return ret;
}

ds18x20Dev ds18x20GetDev(uint8_t num)
{
	return devs[num];
}

uint8_t getDevCount(void)
{
	return devCount;
}
