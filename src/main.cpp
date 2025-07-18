#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <stdio.h> //sprintf()
#include <string> //std::string
#include <vector> //std::vector
//
#include <coreinit/screen.h>
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/title.h>
#include <vpad/input.h>

#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>

#include "draw.hpp"
#include "utils.hpp"

#include <coreinit/bsp.h>
#include <coreinit/foreground.h>
#include <coreinit/ios.h>
#include <coreinit/title.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <nn/fp.h>
#include <vpad/input.h>
#include <coreinit/time.h>
#include <coreinit/thread.h>

#include <mocha/mocha.h>

const int FRAMES_TO_SHOW = 240;

size_t tvBufferSize;
size_t drcBufferSize;

void* tvBuffer;
void* drcBuffer;

VPADStatus vpad;
VPADReadError vError;

uint8_t menu = 0;

#define OPTIONS_MAX 3
uint8_t menuIndex = 0;
static const char* indexOptions[]{
	"Controllers (TODO)",
	"BSP Menu (TODO)",
	"Disc Drive Menu"
};

void endRefresh() {
	if (menu > 1 && menu != OPTIONS_MAX) //no main, no buttons and no exit
		write(48, 17, "Press B to return");

	DCFlushRange(tvBuffer, tvBufferSize);
	DCFlushRange(drcBuffer, drcBufferSize);

	OSScreenFlipBuffersEx(SCREEN_TV);
	OSScreenFlipBuffersEx(SCREEN_DRC);
}
bool startCleanRefresh() {
	VPADRead(VPAD_CHAN_0, &vpad, 1, &vError);

	switch (vError) {
	case VPAD_READ_SUCCESS:
		write(15, 1, "");
		return true;
	case VPAD_READ_NO_SAMPLES:
		write(15, 1, "Waiting for samples...");
		return false;
	case VPAD_READ_INVALID_CONTROLLER:
		write(15, 1, "Invalid controller");
		return false;
	default:
		swrite(15, 1, std::string("Unknown error: ") + hex_tostring(vError, 8));
		return false;
	}
}
bool startRefresh() {
	OSScreenClearBufferEx(SCREEN_TV, 0x00000000);
	OSScreenClearBufferEx(SCREEN_DRC, 0x00000000);

	write(0, 0, "Flatwhite - WUP Hardware Interface by Spletz");
	write(0, 1, "Thanks to drc-test by Pokes303, EjectDisc by Lynx64 and Wii U Video Mode Changer by Lynx64 and FIX94");
	if (menu > 0)
		swrite(0, 2, indexOptions[menu - 1]);

	return startCleanRefresh();
}



//Thanks https://github.com/Lynx64/EjectDisc

void giveFanPpcPermissions() {
	// entity & attribute struct: https://github.com/NWPlayer123/IOSU/blob/master/ios_bsp/libraries/bsp_entity/include/bsp_entity.h
	// GPIO attributes array: 0xE604331C
	uint32_t permissions = 0;
	// +1,364 for FanSpeed, +8 for permissions
	Mocha_IOSUKernelRead32(0xE604331C + 1364 + 8, &permissions);
	// by default FanSpeed has perms 0xFF (BSP_PERMISSIONS_IOS)
	Mocha_IOSUKernelWrite32(0xE604331C + 1364 + 8, permissions | 0xFFFF); // BSP_PERMISSIONS_PPC_ALL
}

void giveEjectRequestPpcPermissions() {
	// entity & attribute struct: https://github.com/NWPlayer123/IOSU/blob/master/ios_bsp/libraries/bsp_entity/include/bsp_entity.h
	// SMC entity is at 0xE6040D94
	// SMC attributes array: 0xE6044364
	uint32_t permissions = 0;
	// +44 for 2nd attribute (which is EjectRequest), +8 for permissions
	Mocha_IOSUKernelRead32(0xE6044364 + 44 + 8, &permissions);
	// by default EjectRequest has perms 0xFF (BSP_PERMISSIONS_IOS)
	Mocha_IOSUKernelWrite32(0xE6044364 + 44 + 8, permissions | 0xF00); // BSP_PERMISSIONS_PPC_USER
}

bool sameFrame = false;
bool checkReturn() {
	if (vpad.trigger & VPAD_BUTTON_B && !sameFrame) {
		sameFrame = true;
		return true;
	}
	sameFrame = false;
	return false;
}

const char* odmStatus[]{
	"NONE",
	"INITIAL",
	"AUTHENTICATION",
	"WAIT_FOR_DISC_READY",
	"CAFE_DISC",
	"RVL_DISC",
	"CLEANING_DISC",
	"UNDEFINED",
	"INVALID_DISC",
	"DIRTY_DISC",
	"NO_DISC",
	"INVALID_DRIVE",
	"FATAL",
	"HARD_FATAL",
	"SHUTDOWN",
};

const char* regionName[]{
	"JAP/CHN (Japan / Taiwan)",
	"USA",
	"PAL",
	"INVALID (Region byte value 0x3 doesn't exist!)",
	"KOR",
};

int odm_handle;

bool menuDisc() {
Mocha_InitLibrary();
int odmStatusIndex;
int discRegion;
int selection = 0;
int discMenuNum = 4;
int statusFrames = 0;
std::string resultMsg;
std::string dsidMsg;
std::string rgnMsg;
uint32_t DSID = 0;
int prevOdmStatusIndex1 = -1;
int prevOdmStatusIndex2 = -1;

		//open '/dev/odm'
		odm_handle = IOS_Open("/dev/odm", IOS_OPEN_READ);
		if (odm_handle < 0) {
			swrite(0, 14, std::string("Failed to open /dev/odm! IOS Error: ") + std::to_string(odm_handle));
		}
	while (WHBProcIsRunning()) {
		if (!startRefresh()) {
			continue;
		}

		write(1, 4, "Turn disc drive motor ON");
		write(1, 5, "Turn disc drive motor OFF");
		write(1, 6, "Awake disc drive motor");
		write(1, 7, "Eject disc");
		write(0, 4 + selection, ">");
		swrite(0, 10, dsidMsg);
		swrite(0, 11, rgnMsg);

		if (statusFrames > 0) {
            swrite(0, 13, resultMsg);
            --statusFrames;
        }
		//get ODM state
		if (!(odm_handle < 0)) {
			alignas(64) uint32_t io_buffer[0x40 / 4];
			uint16_t state = 0;
			uint16_t buf = 0;
			io_buffer[0] = state;
			io_buffer[1] = buf;
			// 'st' means 'state'
			IOSError st_ioctlResult = IOS_Ioctl(odm_handle, 0x04, io_buffer, sizeof(io_buffer), io_buffer, 0x20);
			if (st_ioctlResult == IOS_ERROR_OK) {
				odmStatusIndex = io_buffer[0];
				swrite(0, 9, std::string("ODM State: ") + std::string(odmStatus[odmStatusIndex]));
		}
			else {
				swrite(0, 9, std::string("Unable to obtain ODM state! IOS Error: ") + std::to_string(st_ioctlResult));
		}
	}
		//read disc serial ID
		if (!(odm_handle < 0)) {
		 if ((odmStatusIndex != prevOdmStatusIndex1) && (odmStatusIndex != 0x3)) {
                prevOdmStatusIndex1 = odmStatusIndex; 
			alignas(64) uint32_t io_buffer[0x40 / 4];
			uint16_t command = 1;
			uint16_t buf = 0;
			io_buffer[0] = command;
			io_buffer[1] = buf;
			// 'dsid' means 'disc serial ID'
			IOSError dsid_ioctlResult = IOS_Ioctl(odm_handle, 0x06, io_buffer, sizeof(io_buffer), io_buffer, 0x20);
			if (dsid_ioctlResult == IOS_ERROR_OK) {
				DSID = io_buffer[1];
				dsidMsg = ("Disc Serial ID: " + std::to_string(DSID));
				}
			else if (dsid_ioctlResult == -921601) {
				dsidMsg = "Disc Serial ID: No Cafe disc inserted";
				}
			else {
				dsidMsg = (std::string("Unable to obtain disc serial ID! IOS Error: ") + std::to_string(dsid_ioctlResult));
			}
		}
	}
	else {
		dsidMsg = "Cannot obtain disc serial ID: /dev/odm is not open!";
	}
	//get RVL disc region
	if (!(odm_handle < 0)) {
		if ((odmStatusIndex != prevOdmStatusIndex2)) {
			prevOdmStatusIndex2 = odmStatusIndex;
			if (odmStatusIndex == 0x5) {
			alignas(64) uint32_t io_buffer[0x40 / 4];
			uint16_t region = 0;
			uint16_t buf = 0;
			io_buffer[0] = region;
			io_buffer[1] = buf;
			// 'rgn' means 'region'
			IOSError rgn_ioctlResult = IOS_Ioctl(odm_handle, 0x07, io_buffer, sizeof(io_buffer), io_buffer, 0x20);
			if (rgn_ioctlResult == IOS_ERROR_OK) {
				discRegion = io_buffer[0];
				rgnMsg = (std::string("RVL Disc Region: ") + std::string(regionName[discRegion]));
		}
			else {
				rgnMsg = (std::string("Unable to obtain RVL disc region! IOS Error: ") + std::to_string(rgn_ioctlResult));
		}
	}
	else {
		rgnMsg = "RVL Disc Region: No RVL disc inserted";
		}
	}
	}
	else {
		rgnMsg = "Cannot obtain RVL disc region: /dev/odm is not open!";
	}

	
			switch (vpad.trigger) {
		case VPAD_BUTTON_A:
		if (selection == 0 || selection == 1) {
			if (!(odm_handle < 0)) {
				alignas(64) uint32_t buffer_io[0x40 / 4];
				uint16_t command = (selection == 0) ? 1 : 2;
				uint16_t buf = 0;
				buffer_io[0] = command;
				buffer_io[1] = buf;
				// 'cm' means 'control motor'
				IOSError cm_ioctlResult = IOS_Ioctl(odm_handle, 0x05, buffer_io, sizeof(buffer_io), buffer_io, 0x20);
				const char* action;
				action = (command == 1) ? "start" : "stop";
				if (cm_ioctlResult == IOS_ERROR_OK) {
					resultMsg = std::string("Successfully sent ") + std::string(action) + std::string(" motor command to the disc drive.");
					statusFrames = FRAMES_TO_SHOW;
				}
				else {
					resultMsg = std::string("Failed to send ") + std::string(action) + std::string(" motor command to the disc drive motor! IOS Error: ") + std::to_string(cm_ioctlResult);
					statusFrames = FRAMES_TO_SHOW;
				}
			}
			else {
				resultMsg = "Cannot control motor: /dev/odm is not open!";
				statusFrames = FRAMES_TO_SHOW;
				}
		}	
			else if (selection == 2) {
				if(!(odm_handle < 0)) {
				alignas(64) uint32_t buffer_io[0x40 / 4];
				uint16_t inbuf = 0;
				uint16_t outbuf = 0;
				buffer_io[0] = inbuf;
				buffer_io[1] = outbuf;
				// 'aw' means 'awake'
				IOSError aw_ioctlResult = IOS_Ioctl(odm_handle, 0x02, buffer_io, sizeof(buffer_io), buffer_io, 0x20);
				if (aw_ioctlResult == IOS_ERROR_OK) {
					resultMsg = "Successfully sent awake command to the disc drive motor.";
					statusFrames = FRAMES_TO_SHOW;
				}
				else
				resultMsg = std::string("Failed to send awake command to the disc drive. IOS Error: ") + std::to_string(aw_ioctlResult);
				statusFrames = FRAMES_TO_SHOW;
			}
			else resultMsg = "Cannot control motor: /dev/odm is not open!";
			}
			else if (selection == 3) {
				giveEjectRequestPpcPermissions();
				uint32_t request = 1;
				BSPError bspRval = bspWrite("SMC", 0, "EjectRequest", 4, &request);
			 	if (bspRval == 0) {
					resultMsg = std::string("Successfully stimulated eject request.");
					statusFrames = FRAMES_TO_SHOW;
					}
			else {
				resultMsg = ("Failed to stimulate eject request! BSP_RVAL: ") + std::to_string(bspRval);
				statusFrames = FRAMES_TO_SHOW;
				}
			}
			break;
		case VPAD_BUTTON_UP:
		case VPAD_STICK_L_EMULATION_UP:
			selection = (selection + discMenuNum - 1) % discMenuNum;
			break;
		case VPAD_BUTTON_DOWN:
		case VPAD_STICK_L_EMULATION_DOWN:
			selection = (selection + 1) % discMenuNum;
			break;
		}

		if (checkReturn())
		 return true;
		endRefresh();
}
	return false;
}



nn::act::PrincipalId pid;
const size_t MAX_REQUESTS = 100;
const size_t MAX_MESSAGES = 100;
nn::act::PrincipalId pidList[MAX_REQUESTS];
uint64_t midList[MAX_MESSAGES];
uint32_t numRetrieved = 0;
uint32_t errorCode;

void OnFriendRequestAccepted(nn::Result result, void* pContext) {
	if ((result & 0x80000000) == 0) {
		write(0, 8, "Successfully accepted friend request in first slot.");
	}
	else {
		errorCode = nn::fp::ResultToErrorCode(result);
		swrite(0, 8, std::string("Failed to accept friend request. Error Code: ") + std::to_string(errorCode));
	}
}

bool menuTest() {
		nn::fp::Initialize();
	while (WHBProcIsRunning()) {
		if (!startRefresh())
			continue;
		pid = nn::fp::GetMyPrincipalId();
		swrite(0, 3, std::string("Current user PID: ") + std::to_string(pid));
		nn::Result result = nn::fp::GetFriendRequestList(pidList, &numRetrieved, 0, MAX_REQUESTS);
		if (numRetrieved !=0) {
		swrite(0, 4, std::string("Number of recieved friend requests pending: ") + std::to_string(numRetrieved));
		swrite(0, 5, std::string("Friend request in first slot: ") + std::to_string(pidList[0]));
		result = nn::fp::GetFriendRequestMessageId(midList, pidList, numRetrieved);
		swrite(0, 6, std::string("Message ID of request in first slot: ") + std::to_string(midList[0]));
		result = nn::fp::AcceptFriendRequestAsync(midList[0], &OnFriendRequestAccepted, nullptr);
		}

		if (checkReturn())
			return true;

		endRefresh();
	}
	return false;
}

bool fanOn = true;          
int  selection = 0;
uint32_t fanRequest;
BSPError bspRval;
std::string resultMsg;
int msgFrames = 0;
const int TIME_TO_SHOW = 240;
int fanTimer;

BSPError toggleFan(uint32_t fanRequest) {
				giveFanPpcPermissions();
				return bspWrite("GPIO", 0, "FanSpeed", 4, &fanRequest);
}

bool menuBSP() {
Mocha_InitLibrary();
fanRequest = 0;
bool fanCountdownActive = false;
	while (WHBProcIsRunning()) {
		if (!startRefresh())
			continue;

        std::string line = ("> " + std::string(fanOn ? "Turn fan OFF" : "Turn fan ON"));

        swrite(0, 4, line);

        if (vpad.trigger & VPAD_BUTTON_A) {
			fanRequest = (fanOn ? 0 : 1);
            bspRval = toggleFan(fanRequest);
			if (bspRval == BSP_ERROR_OK) {
				resultMsg = ("Successfully changed fan speed to " + std::string(fanOn ? "0" : "1"));
				fanOn = (fanOn ? false : true);
				if (fanRequest == 0) {
					fanCountdownActive = true;
					fanTimer = 10;
				}
			}
			else {
				resultMsg = (std::string("Failed to change fan speed. BSP Error: ") + std::to_string(bspRval));
			}
			msgFrames = TIME_TO_SHOW;
        }

		if (msgFrames > 0) {
            swrite(0, 7, resultMsg);
            --msgFrames;
        }

		if (fanCountdownActive == true) {
			swrite(0, 10, ("WARNING: Fan will be turned back on in " + std::to_string(fanTimer)));
			//will implement this so that it is interruptable and overwritable
			OSSleepTicks(OSMillisecondsToTicks(1000));
			--fanTimer;
			if (fanTimer < 0) {
			bspRval = toggleFan(1);
				if (bspRval == BSP_ERROR_OK){
				resultMsg = "Turned fan back on";
				}
				else {
					resultMsg = "FAILED TO TURN FAN BACK ON. HOLD THE POWER BUTTON TO SHUT DOWN THE CONSOLE.";
				}
			fanOn = (fanOn ? true : false);
			fanCountdownActive = false;
			}
			msgFrames = TIME_TO_SHOW;
		}


		if (checkReturn() && (fanCountdownActive == false))
			return true;

        endRefresh();
	}
	return false;
}

bool menuController() {
	while (WHBProcIsRunning()) {
		if (!startRefresh())
			continue;	

		if (checkReturn())
			return true;
		
		endRefresh();
	}
	return false;
}

void displayIndex() {
	for (int i = 0; i < OPTIONS_MAX; i++) {
		if (i == menuIndex)
			write(0, i + 3, ">");
		write(1, i + 3, indexOptions[i]);
	}
}

int main() {
	WHBProcInit();
	OSScreenInit();

	tvBufferSize = OSScreenGetBufferSizeEx(SCREEN_TV);
	drcBufferSize = OSScreenGetBufferSizeEx(SCREEN_DRC);

	tvBuffer = memalign(0x100, tvBufferSize);
	drcBuffer = memalign(0x100, drcBufferSize);

	if (!tvBuffer || !drcBuffer) {
		if (tvBuffer) free(tvBuffer);
		if (drcBuffer) free(drcBuffer);

		OSScreenShutdown();

		return 1;
	}

	OSScreenSetBufferEx(SCREEN_TV, tvBuffer);
	OSScreenSetBufferEx(SCREEN_DRC, drcBuffer);

	OSScreenEnableEx(SCREEN_TV, true);
	OSScreenEnableEx(SCREEN_DRC, true);

	VPADInit();

	while (WHBProcIsRunning()) {
		if (!startRefresh())
			goto refreshBuffs;

		switch (menu) {
		case 0:
			displayIndex();

			switch (vpad.trigger) {
			case VPAD_BUTTON_A:
				menu = menuIndex + 1;
				goto refreshBuffs;
			case VPAD_BUTTON_B:
				goto exit;
			case VPAD_BUTTON_UP:
			case VPAD_STICK_L_EMULATION_UP:
				if (menuIndex > 0)
					menuIndex--;
				else
					menuIndex = OPTIONS_MAX - 1;
				break;
			case VPAD_BUTTON_DOWN:
			case VPAD_STICK_L_EMULATION_DOWN:
				if (menuIndex < OPTIONS_MAX - 1)
					menuIndex++;
				else
					menuIndex = 0;
				break;
			}
			break;
		case 1: //Controllers
			if (!menuController())
				goto exit;
			goto endMenu;
		case 2: //BSP
			if(!menuBSP())
				goto exit;
			Mocha_DeInitLibrary();
			goto endMenu;
		case 3: //Disc Drive
			if (!menuDisc())
				goto exit;
			Mocha_DeInitLibrary();
			IOS_Close(odm_handle);
			goto endMenu;
		}
		
		write(50, 17, "Press B to exit");
		write(39, 16, "Press HOME to exit anytime");

		endMenu:
		menu = 0;
		
		refreshBuffs:
		endRefresh();
	}
exit:
	if (tvBuffer) free(tvBuffer);
	if (drcBuffer) free(drcBuffer);

	OSScreenShutdown();
	WHBProcShutdown();

	return 1;
}
