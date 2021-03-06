/*
 * =====================================================================================
 *
 *       Filename:  hostctrl.c
 *
 *    Description:  上位机控制请求处理
 *
 *        Version:  
 *        Created:  
 *       Revision:  
 *       Compiler:  
 *
 *         Author:  zhangyuxiang
 *   Organization:  
 *
 * =====================================================================================
 */

#include "common.h"
#include "systick.h"
#include "hostctrl.h"
#include "usart.h"
#include "led.h"
#include "command.h"
#include "move.h"
#include "heatbed.h"
#include "extruder.h"
#include "gfiles.h"
#include "fanControl.h"
#include <string.h>
#include <stdlib.h>

#define CMD_BUF_LEN 8
#define PARAM_BUF_LEN 8

enum {PARSE_INITIAL, PARSE_CMD, PARSE_PARAM};
static uint8_t parse_stage;
static bool cmd_received;
static char cmd_buf[CMD_BUF_LEN], param_buf[PARAM_BUF_LEN];

void HostCtrl_Init(void)
{
	// USART_RxInt_Config(true);
	parse_stage = PARSE_INITIAL;
	cmd_received = false;
}

bool HostCtrl_GetCmd(char **p_cmd, char **p_param)
{
	if(cmd_received){
		*p_param = param_buf;
		*p_cmd = cmd_buf;
	}
	return cmd_received;
}

//指令已经处理完,准备接收下一指令
void HostCtrl_CmdProcessed()
{
	cmd_received = false;
}

static void parse_host_cmd(uint8_t byte)
{
	static int cmd_buf_i = 0, param_buf_i = 0;
	switch(parse_stage){
		case PARSE_INITIAL:
			//如果有尚未处理的指令,则丢弃本次指令
			if(byte == '!' && !cmd_received){
				parse_stage = PARSE_CMD;
				cmd_buf_i = param_buf_i = 0;
			}
			break;
		case PARSE_CMD:
			if('A'<=byte && byte<='Z'){
				if(cmd_buf_i < CMD_BUF_LEN-2)
					cmd_buf[cmd_buf_i++] = byte;
			}else if('#' == byte){
				cmd_buf[cmd_buf_i] = '\0';
				// cmd_received = true;
				parse_stage = PARSE_PARAM;
			}else{
				//无效指令
				parse_stage = PARSE_INITIAL;
			}
			break;
		case PARSE_PARAM:
			if('\r' == byte || '\n' == byte){
				param_buf[param_buf_i] = '\0';
				cmd_received = true;
				parse_stage = PARSE_INITIAL;
			}else{
				if(param_buf_i < PARAM_BUF_LEN-2)
					param_buf[param_buf_i++] = byte;
			}
			break;
	}
}

static void reportState()
{
	uint8_t b;
	int16_t temp;
	uint16_t state;
	uint8_t progress;
	int output;

	Command_GetState(&b, &state, &progress);
	REPORT(INFO_PRINT, "%d,%d,%d", (int)b, (int)state, (int)progress);

	Extruder_GetState(&temp, &output, &b);
	REPORT(INFO_EXTRUDER, "%d,%d,%d", (int)temp, (int)output, (int)b);

	HeatBed_GetState(&temp, &output, &b);
	REPORT(INFO_HEATBED, "%d,%d,%d", (int)temp, (int)output, (int)b);

}

//处理上位机请求
static void processRequest(char* cmd, char* param)
{
	static char (*files)[][SD_MAX_FILENAME_LEN] = NULL;
	DBG_MSG("Cmd: %s, Param: %s", cmd, param);
	if(strcmp(cmd, "QRY") == 0){
		reportState();
	}else if(strcmp(cmd, "STOP") == 0){
		bool ret = Command_StopPrinting();
		REPORT(INFO_REPLY, "%d", ret);
	}else if(strcmp(cmd, "LIST") == 0){
		files = FileManager_ListGFiles();
		if(files != NULL){
			for(int i=0; i<SD_MAX_ITEMS; i++){
				if(!(*files)[i][0])
					break;
				REPORT(INFO_LIST_FILES, "%s", (*files)[i]);
			}
		}
	}else if(strcmp(cmd, "START") == 0){
		int num = atoi(param);
		if(num >= 0 && num < SD_MAX_ITEMS){
			bool ret = Command_StartPrinting((*files)[num]);
			REPORT(INFO_REPLY, "%d", ret);
		}
	}else if(strcmp(cmd, "DBG") == 0){
		uint8_t tmp;
		int val[4] = {0};
		if(!Command_IsStandBy()){
			REPORT(INFO_REPLY, "0", 0);
		}else{
			int result = 1;
			switch(*param){
				case 'X':
				case 'Y':
				case 'Z':
				case 'A':
					if(*param == 'A')
						tmp = 3;
					else
						tmp = *param-'X';
					val[tmp] = atoi(param+1)*1000; //um->mm
					Motor_PowerOn();
					result = Move_RelativeMove(val, DEFAULT_FEEDRATE);
					break;
				case 'e':
					Extruder_SetOutput(atoi(param+1));
					break;
				case 'E':
					if(*(param+1)){
						if(*(param+1) == '-'){
							Extruder_Stop_Heating();
						}else{
							Extruder_Start_Heating(atoi(param+1));
						}
					}
					break;
				case 'h':
					HeatBed_SetOutput(atoi(param+1));
					break;
				case 'H':
					if(*(param+1)){
						if(*(param+1) == '-'){
							HeatBed_Stop_Heating();
						}else{
							HeatBed_Start_Heating(atoi(param+1));
						}
					}
					break;
				case 'f':
					Fan_Enable(*(param+1) == '1');
					break;
			}
			REPORT(INFO_REPLY, "%d", result);
		}
	}else if(strcmp(cmd, "HOME") == 0){
		if(!Command_IsStandBy()){
			REPORT(INFO_REPLY, "0", 0);
		}else{
			int result = 0;
			if(strcmp(param, "XY") == 0){
				result = Command_ManuallyHome(MOVE_DIR_X|MOVE_DIR_Y);
			}else if(strcmp(param, "Z") == 0){
				result = Command_ManuallyHome(MOVE_DIR_Z);
			}
			REPORT(INFO_REPLY, "%d", result);
		}
	}
}

static void fetchHostCmd(void)
{
	static SysTick_t last_report = 0;
	static uint8_t led_state = LED_ON;

	char *p_cmd, *p_param;
	SysTick_t now = GetSystemTick();
	if(now - last_report > REPORT_PERIOD){

		last_report = now;

		LED_Enable(LED1, led_state);
		led_state = (led_state == LED_ON ? LED_OFF : LED_ON);
	}

	if(HostCtrl_GetCmd(&p_cmd, &p_param)){
		processRequest(p_cmd, p_param);
		HostCtrl_CmdProcessed();
	}
}

void HostCtrl_Task(void)
{
	if(USART_GetFlagStatus(BT_USART, USART_FLAG_RXNE) == SET){
		uint8_t byte = USART_getchar(BT_USART);
		parse_host_cmd(byte);
	}

	fetchHostCmd();
}

void HostCtrl_Interrupt(void)
{
	uint8_t byte = USART_ReceiveData(BT_USART);
	USART_putchar(BT_USART, byte);
	// USART_ClearITPendingBit(BT_USART, USART_IT_RXNE);
}