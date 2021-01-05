#ifndef RADIO_H_
#define RADIO_H_

#define RADIO_MAX_PACKET_LENGTH     64u

typedef struct
{
	uint8_t   *Radio_ConfigurationArray;

	uint8_t   Radio_ChannelNumber;
	uint8_t   Radio_PacketLength;
	uint8_t   Radio_State_After_Power_Up;

	uint16_t  Radio_Delay_Cnt_After_Reset;

	uint8_t   Radio_CustomPayload[RADIO_MAX_PACKET_LENGTH];
} tRadioConfiguration;



#endif /* RADIO_H_ */