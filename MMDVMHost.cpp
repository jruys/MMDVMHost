/*
 *   Copyright (C) 2015,2016 by Jonathan Naylor G4KLX
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "MMDVMHost.h"
#include "Log.h"
#include "Version.h"
#include "StopWatch.h"
#include "Defines.h"
#include "DStarControl.h"
#include "DMRControl.h"
#include "TFTSerial.h"
#include "NullDisplay.h"
#include "YSFControl.h"
#include "Nextion.h"

#if defined(HD44780)
#include "HD44780.h"
#endif

#include <cstdio>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "MMDVM.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/MMDVM.ini";
#endif

static bool m_killed = false;
static int  m_signal = 0;

#if !defined(_WIN32) && !defined(_WIN64)
static void sigHandler(int signum)
{
  m_killed = true;
  m_signal = signum;
}
#endif

const char* HEADER1 = "This software is for use on amateur radio networks only,";
const char* HEADER2 = "it is to be used for educational purposes only. Its use on";
const char* HEADER3 = "commercial networks is strictly prohibited.";
const char* HEADER4 = "Copyright(C) 2015, 2016 by Jonathan Naylor, G4KLX and others";

int main(int argc, char** argv)
{
  const char* iniFile = DEFAULT_INI_FILE;
  if (argc > 1)
	iniFile = argv[1];

#if !defined(_WIN32) && !defined(_WIN64)
  ::signal(SIGTERM, sigHandler);
  ::signal(SIGHUP,  sigHandler);
#endif

  int ret = 0;

  do {
	  m_signal = 0;

	  CMMDVMHost* host = new CMMDVMHost(std::string(iniFile));
	  ret = host->run();

	  delete host;

	  if (m_signal == 15)
		  ::LogInfo("Caught SIGTERM, exiting");

	  if (m_signal == 1)
		  ::LogInfo("Caught SIGHUP, restarting");
  } while (m_signal == 1);

  ::LogFinalise();

  return ret;
}

CMMDVMHost::CMMDVMHost(const std::string& confFile) :
m_conf(confFile),
m_modem(NULL),
m_dstarNetwork(NULL),
m_dmrNetwork(NULL),
m_display(NULL),
m_mode(MODE_IDLE),
m_modeTimer(1000U),
m_dmrTXTimer(1000U),
m_duplex(false),
m_dstarEnabled(false),
m_dmrEnabled(false),
m_ysfEnabled(false)
{
}

CMMDVMHost::~CMMDVMHost()
{
}

int CMMDVMHost::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "MMDVMHost: cannot read the .ini file\n");
		return 1;
	}

	ret = ::LogInitialise(m_conf.getLogFilePath(), m_conf.getLogFileRoot(), m_conf.getLogFileLevel(), m_conf.getLogDisplayLevel());
	if (!ret) {
		::fprintf(stderr, "MMDVMHost: unable to open the log file\n");
		return 1;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			    ::LogWarning("Couldn't fork() , exiting");
			    return -1;
		    }
		else if (pid != 0)
			exit(EXIT_SUCCESS);

		// Create new session and process group
		if (::setsid() == -1){
			    ::LogWarning("Couldn't setsid(), exiting");
			    return -1;
		    }

		// Set the working directory to the root directory
		if (::chdir("/") == -1){
			    ::LogWarning("Couldn't cd /, exiting");
			    return -1;
		    }

		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);
#if !defined(HD44780)
		//If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == NULL) {
				::LogError("Could not get the mmdvm user, exiting");
				return -1;
			}
			
			uid_t mmdvm_uid = user->pw_uid;
		    gid_t mmdvm_gid = user->pw_gid;

		    //Set user and group ID's to mmdvm:mmdvm
		    if (setgid(mmdvm_gid) != 0) {
			    ::LogWarning("Could not set mmdvm GID, exiting");
			    return -1;
		    }

			if (setuid(mmdvm_uid) != 0) {
			    ::LogWarning("Could not set mmdvm UID, exiting");
			    return -1;
		    }
		    
		    //Double check it worked (AKA Paranoia) 
		    if (setuid(0) != -1){
			    ::LogWarning("It's possible to regain root - something is wrong!, exiting");
			    return -1;
		    }
		
		}
	}
#else
	::LogWarning("Dropping root permissions in daemon mode is disabled with HD44780 display");
	}
#endif
#endif

	LogInfo(HEADER1);
	LogInfo(HEADER2);
	LogInfo(HEADER3);
	LogInfo(HEADER4);

	LogMessage("MMDVMHost-%s is starting", VERSION);

	readParams();

	ret = createModem();
	if (!ret)
		return 1;

	createDisplay();

	if (m_dstarEnabled && m_conf.getDStarNetworkEnabled()) {
		ret = createDStarNetwork();
		if (!ret)
			return 1;
	}

	if (m_dmrEnabled && m_conf.getDMRNetworkEnabled()) {
		ret = createDMRNetwork();
		if (!ret)
			return 1;
	}

	CTimer dmrBeaconTimer(1000U, 4U);
	bool dmrBeaconsEnabled = m_dmrEnabled && m_conf.getDMRBeacons();

	CStopWatch stopWatch;
	stopWatch.start();

	CDStarControl* dstar = NULL;
	if (m_dstarEnabled) {
		std::string callsign = m_conf.getCallsign();
		std::string module = m_conf.getDStarModule();
		bool selfOnly = m_conf.getDStarSelfOnly();
		unsigned int timeout = m_conf.getTimeout();
		std::vector<std::string> blackList = m_conf.getDStarBlackList();

		LogInfo("D-Star Parameters");
		LogInfo("    Callsign: %s", callsign.c_str());
		LogInfo("    Module: %s", module.c_str());
		LogInfo("    Self Only: %s", selfOnly ? "yes" : "no");
		if (blackList.size() > 0U)
			LogInfo("    Black List: %u", blackList.size());
		LogInfo("    Timeout: %us", timeout);

		dstar = new CDStarControl(callsign, module, selfOnly, blackList, m_dstarNetwork, m_display, timeout, m_duplex);
	}

	CDMRControl* dmr = NULL;
	if (m_dmrEnabled) {
		unsigned int id        = m_conf.getDMRId();
		unsigned int colorCode = m_conf.getDMRColorCode();
		bool selfOnly          = m_conf.getDMRSelfOnly();
		std::vector<unsigned int> prefixes = m_conf.getDMRPrefixes();
		std::vector<unsigned int> blackList = m_conf.getDMRBlackList();
		unsigned int timeout   = m_conf.getTimeout();
		std::string lookupFile = m_conf.getDMRLookupFile();
		unsigned int txHang    = m_conf.getDMRTXHang();

		LogInfo("DMR Parameters");
		LogInfo("    Id: %u", id);
		LogInfo("    Color Code: %u", colorCode);
		LogInfo("    Self Only: %s", selfOnly ? "yes" : "no");
		LogInfo("    Prefixes: %u", prefixes.size());
		if (blackList.size() > 0U)
			LogInfo("    Black List: %u", blackList.size());
		LogInfo("    Timeout: %us", timeout);
		LogInfo("    Lookup File: %s", lookupFile.length() > 0U ? lookupFile.c_str() : "None");
		LogInfo("    TX Hang: %us", txHang);

		dmr = new CDMRControl(id, colorCode, selfOnly, prefixes, blackList, timeout, m_modem, m_dmrNetwork, m_display, m_duplex, lookupFile);

		m_dmrTXTimer.setTimeout(txHang);
	}

	CYSFControl* ysf = NULL;
	if (m_ysfEnabled) {
		std::string callsign = m_conf.getCallsign();
		unsigned int timeout = m_conf.getTimeout();
		bool parrot          = m_conf.getFusionParrotEnabled();

		LogInfo("System Fusion Parameters");
		LogInfo("    Callsign: %s", callsign.c_str());
		LogInfo("    Timeout: %us", timeout);
		LogInfo("    Parrot: %s", parrot ? "enabled" : "disabled");

		ysf = new CYSFControl(callsign, m_display, timeout, m_duplex, parrot);
	}

	m_modeTimer.setTimeout(m_conf.getModeHang());

	setMode(MODE_IDLE);

	while (!m_killed) {
		bool lockout = m_modem->hasLockout();
		if (lockout && m_mode != MODE_LOCKOUT)
			setMode(MODE_LOCKOUT);
		else if (!lockout && m_mode == MODE_LOCKOUT)
			setMode(MODE_IDLE);

		bool error = m_modem->hasError();
		if (error && m_mode != MODE_ERROR)
			setMode(MODE_ERROR);
		else if (!error && m_mode == MODE_ERROR)
			setMode(MODE_IDLE);

		unsigned char data[200U];
		unsigned int len;
		bool ret;

		len = m_modem->readDStarData(data);
		if (dstar != NULL && len > 0U) {
			if (m_mode == MODE_IDLE) {
				bool ret = dstar->writeModem(data);
				if (ret)
					setMode(MODE_DSTAR);
			} else if (m_mode == MODE_DSTAR) {
				dstar->writeModem(data);
				m_modeTimer.start();
			} else if (m_mode != MODE_LOCKOUT) {
				LogWarning("D-Star modem data received when in mode %u", m_mode);
			}
		}

		len = m_modem->readDMRData1(data);
		if (dmr != NULL && len > 0U) {
			if (m_mode == MODE_IDLE) {
				if (m_duplex) {
					bool ret = dmr->processWakeup(data);
					if (ret) {
						setMode(MODE_DMR);
						dmrBeaconTimer.stop();
					}
				} else {
					setMode(MODE_DMR);
					dmr->writeModemSlot1(data);
					dmrBeaconTimer.stop();
				}
			} else if (m_mode == MODE_DMR) {
				if (m_duplex && !m_modem->hasTX()) {
					bool ret = dmr->processWakeup(data);
					if (ret) {
						m_modem->writeDMRStart(true);
						m_dmrTXTimer.start();
					}
				} else {
					dmr->writeModemSlot1(data);
					dmrBeaconTimer.stop();
					m_modeTimer.start();
					if (m_duplex)
						m_dmrTXTimer.start();
				}
			} else if (m_mode != MODE_LOCKOUT) {
				LogWarning("DMR modem data received when in mode %u", m_mode);
			}
		}

		len = m_modem->readDMRData2(data);
		if (dmr != NULL && len > 0U) {
			if (m_mode == MODE_IDLE) {
				if (m_duplex) {
					bool ret = dmr->processWakeup(data);
					if (ret) {
						setMode(MODE_DMR);
						dmrBeaconTimer.stop();
					}
				} else {
					setMode(MODE_DMR);
					dmr->writeModemSlot2(data);
					dmrBeaconTimer.stop();
				}
			} else if (m_mode == MODE_DMR) {
				if (m_duplex && !m_modem->hasTX()) {
					bool ret = dmr->processWakeup(data);
					if (ret) {
						m_modem->writeDMRStart(true);
						m_dmrTXTimer.start();
					}
				} else {
					dmr->writeModemSlot2(data);
					dmrBeaconTimer.stop();
					m_modeTimer.start();
					if (m_duplex)
						m_dmrTXTimer.start();
				}
			} else if (m_mode != MODE_LOCKOUT) {
				LogWarning("DMR modem data received when in mode %u", m_mode);
			}
		}

		len = m_modem->readYSFData(data);
		if (ysf != NULL && len > 0U) {
			if (m_mode == MODE_IDLE) {
				bool ret = ysf->writeModem(data);
				if (ret)
					setMode(MODE_YSF);
			} else if (m_mode == MODE_YSF) {
				ysf->writeModem(data);
				m_modeTimer.start();
			} else if (m_mode != MODE_LOCKOUT) {
				LogWarning("System Fusion modem data received when in mode %u", m_mode);
			}
		}

		if (m_modeTimer.isRunning() && m_modeTimer.hasExpired())
			setMode(MODE_IDLE);

		if (dstar != NULL) {
			ret = m_modem->hasDStarSpace();
			if (ret) {
				len = dstar->readModem(data);
				if (len > 0U) {
					if (m_mode == MODE_IDLE)
						setMode(MODE_DSTAR);
					if (m_mode == MODE_DSTAR) {
						m_modem->writeDStarData(data, len);
						m_modeTimer.start();
					} else if (m_mode != MODE_LOCKOUT) {
						LogWarning("D-Star data received when in mode %u", m_mode);
					}
				}
			}
		}

		if (dmr != NULL) {
			ret = m_modem->hasDMRSpace1();
			if (ret) {
				len = dmr->readModemSlot1(data);
				if (len > 0U) {
					if (m_mode == MODE_IDLE)
						setMode(MODE_DMR);
					if (m_mode == MODE_DMR) {
						if (m_duplex) {
							m_modem->writeDMRStart(true);
							m_dmrTXTimer.start();
						}
						m_modem->writeDMRData1(data, len);
						dmrBeaconTimer.stop();
						m_modeTimer.start();
					} else if (m_mode != MODE_LOCKOUT) {
						LogWarning("DMR data received when in mode %u", m_mode);
					}
				}
			}

			ret = m_modem->hasDMRSpace2();
			if (ret) {
				len = dmr->readModemSlot2(data);
				if (len > 0U) {
					if (m_mode == MODE_IDLE)
						setMode(MODE_DMR);
					if (m_mode == MODE_DMR) {
						if (m_duplex) {
							m_modem->writeDMRStart(true);
							m_dmrTXTimer.start();
						}
						m_modem->writeDMRData2(data, len);
						dmrBeaconTimer.stop();
						m_modeTimer.start();
					} else if (m_mode != MODE_LOCKOUT) {
						LogWarning("DMR data received when in mode %u", m_mode);
					}
				}
			}
		}

		if (ysf != NULL) {
			ret = m_modem->hasYSFSpace();
			if (ret) {
				len = ysf->readModem(data);
				if (len > 0U) {
					if (m_mode == MODE_IDLE)
						setMode(MODE_YSF);
					if (m_mode == MODE_YSF) {
						m_modem->writeYSFData(data, len);
						m_modeTimer.start();
					} else if (m_mode != MODE_LOCKOUT) {
						LogWarning("System Fusion data received when in mode %u", m_mode);
					}
				}
			}
		}

		if (m_dmrNetwork != NULL) {
			bool run = m_dmrNetwork->wantsBeacon();
			if (dmrBeaconsEnabled && run && m_mode == MODE_IDLE) {
				setMode(MODE_DMR, false);
				dmrBeaconTimer.start();
			}
		}

		unsigned int ms = stopWatch.elapsed();
		stopWatch.start();

		m_modem->clock(ms);
		m_modeTimer.clock(ms);

		if (dstar != NULL)
			dstar->clock();
		if (dmr != NULL)
			dmr->clock();
		if (ysf != NULL)
			ysf->clock();

		if (m_dstarNetwork != NULL)
			m_dstarNetwork->clock(ms);
		if (m_dmrNetwork != NULL)
			m_dmrNetwork->clock(ms);

		dmrBeaconTimer.clock(ms);
		if (dmrBeaconTimer.isRunning() && dmrBeaconTimer.hasExpired()) {
			setMode(MODE_IDLE, false);
			dmrBeaconTimer.stop();
		}

		m_dmrTXTimer.clock(ms);
		if (m_dmrTXTimer.isRunning() && m_dmrTXTimer.hasExpired()) {
			m_modem->writeDMRStart(false);
			m_dmrTXTimer.stop();
		}

		if (ms < 5U) {
#if defined(_WIN32) || defined(_WIN64)
			::Sleep(5UL);		// 5ms
#else
			::usleep(5000);		// 5ms
#endif
		}
	}

	LogMessage("MMDVMHost is exiting on receipt of SIGHUP1");

	setMode(MODE_IDLE);

	m_modem->close();
	delete m_modem;

	m_display->close();
	delete m_display;

	if (m_dstarNetwork != NULL) {
		m_dstarNetwork->close();
		delete m_dstarNetwork;
	}

	if (m_dmrNetwork != NULL) {
		m_dmrNetwork->close();
		delete m_dmrNetwork;
	}

	delete dstar;
	delete dmr;
	delete ysf;

	return 0;
}

bool CMMDVMHost::createModem()
{
    std::string port         = m_conf.getModemPort();
    bool rxInvert            = m_conf.getModemRXInvert();
    bool txInvert            = m_conf.getModemTXInvert();
    bool pttInvert           = m_conf.getModemPTTInvert();
    unsigned int txDelay     = m_conf.getModemTXDelay();
	unsigned int dmrDelay    = m_conf.getModemDMRDelay();
	unsigned int rxLevel     = m_conf.getModemRXLevel();
    unsigned int txLevel     = m_conf.getModemTXLevel();
    bool debug               = m_conf.getModemDebug();
	unsigned int colorCode   = m_conf.getDMRColorCode();
	unsigned int rxFrequency = m_conf.getRxFrequency();
	unsigned int txFrequency = m_conf.getTxFrequency();
	int oscOffset            = m_conf.getModemOscOffset();

	LogInfo("Modem Parameters");
	LogInfo("    Port: %s", port.c_str());
	LogInfo("    RX Invert: %s", rxInvert ? "yes" : "no");
	LogInfo("    TX Invert: %s", txInvert ? "yes" : "no");
	LogInfo("    PTT Invert: %s", pttInvert ? "yes" : "no");
	LogInfo("    TX Delay: %ums", txDelay);
	LogInfo("    DMR Delay: %u (%.1fms)", dmrDelay, float(dmrDelay) * 0.0416666F);
	LogInfo("    RX Level: %u%%", rxLevel);
	LogInfo("    TX Level: %u%%", txLevel);
	LogInfo("    RX Frequency: %uHz", rxFrequency);
	LogInfo("    TX Frequency: %uHz", txFrequency);
	LogInfo("    Osc. Offset: %dppm", oscOffset);

	m_modem = new CModem(port, rxInvert, txInvert, pttInvert, txDelay, rxLevel, txLevel, dmrDelay, oscOffset, debug);
	m_modem->setModeParams(m_dstarEnabled, m_dmrEnabled, m_ysfEnabled);
	m_modem->setRFParams(rxFrequency, txFrequency);
	m_modem->setDMRParams(colorCode);

	bool ret = m_modem->open();
	if (!ret) {
		delete m_modem;
		m_modem = NULL;
		return false;
	}

	return true;
}

bool CMMDVMHost::createDStarNetwork()
{
	std::string gatewayAddress = m_conf.getDStarGatewayAddress();
	unsigned int gatewayPort = m_conf.getDStarGatewayPort();
	unsigned int localPort = m_conf.getDStarLocalPort();
	bool debug = m_conf.getDStarNetworkDebug();

	LogInfo("D-Star Network Parameters");
	LogInfo("    Gateway Address: %s", gatewayAddress.c_str());
	LogInfo("    Gateway Port: %u", gatewayPort);
	LogInfo("    Local Port: %u", localPort);

	m_dstarNetwork = new CDStarNetwork(gatewayAddress, gatewayPort, localPort, m_duplex, VERSION, debug);

	bool ret = m_dstarNetwork->open();
	if (!ret) {
		delete m_dstarNetwork;
		m_dstarNetwork = NULL;
		return false;
	}

	m_dstarNetwork->enable(true);

	return true;
}

bool CMMDVMHost::createDMRNetwork()
{
	std::string address  = m_conf.getDMRNetworkAddress();
	unsigned int port    = m_conf.getDMRNetworkPort();
	unsigned int local   = m_conf.getDMRNetworkLocal();
	unsigned int id      = m_conf.getDMRId();
	std::string password = m_conf.getDMRNetworkPassword();
	bool debug           = m_conf.getDMRNetworkDebug();
	bool slot1           = m_conf.getDMRNetworkSlot1();
	bool slot2           = m_conf.getDMRNetworkSlot2();

	LogInfo("DMR Network Parameters");
	LogInfo("    Address: %s", address.c_str());
	LogInfo("    Port: %u", port);
	if (local > 0U)
		LogInfo("    Local: %u", local);
	else
		LogInfo("    Local: random");
	LogInfo("    Slot 1: %s", slot1 ? "enabled" : "disabled");
	LogInfo("    Slot 2: %s", slot2 ? "enabled" : "disabled");

	m_dmrNetwork = new CDMRIPSC(address, port, local, id, password, m_duplex, VERSION, debug, slot1, slot2);

	std::string callsign     = m_conf.getCallsign();
	unsigned int rxFrequency = m_conf.getRxFrequency();
	unsigned int txFrequency = m_conf.getTxFrequency();
	unsigned int power       = m_conf.getPower();
	unsigned int colorCode   = m_conf.getDMRColorCode();
	float latitude           = m_conf.getLatitude();
	float longitude          = m_conf.getLongitude();
	int height               = m_conf.getHeight();
	std::string location     = m_conf.getLocation();
	std::string description  = m_conf.getDescription();
	std::string url          = m_conf.getURL();

	LogInfo("Info Parameters");
	LogInfo("    Callsign: %s", callsign.c_str());
	LogInfo("    RX Frequency: %uHz", rxFrequency);
	LogInfo("    TX Frequency: %uHz", txFrequency);
	LogInfo("    Power: %uW", power);
	LogInfo("    Latitude: %fdeg N", latitude);
	LogInfo("    Longitude: %fdeg E", longitude);
	LogInfo("    Height: %um", height);
	LogInfo("    Location: \"%s\"", location.c_str());
	LogInfo("    Description: \"%s\"", description.c_str());
	LogInfo("    URL: \"%s\"", url.c_str());

	m_dmrNetwork->setConfig(callsign, rxFrequency, txFrequency, power, colorCode, latitude, longitude, height, location, description, url);

	bool ret = m_dmrNetwork->open();
	if (!ret) {
		delete m_dmrNetwork;
		m_dmrNetwork = NULL;
		return false;
	}

	m_dmrNetwork->enable(true);

	return true;
}

void CMMDVMHost::readParams()
{
	m_dstarEnabled = m_conf.getDStarEnabled();
	m_dmrEnabled   = m_conf.getDMREnabled();
	m_ysfEnabled   = m_conf.getFusionEnabled();
	m_duplex       = m_conf.getDuplex();
}

void CMMDVMHost::createDisplay()
{
	std::string type       = m_conf.getDisplay();
	std::string callsign   = m_conf.getCallsign();
	unsigned int dmrid     = m_conf.getDMRId();

	LogInfo("Display Parameters");
	LogInfo("    Type: %s", type.c_str());

	if (type == "TFT Serial") {
		std::string port        = m_conf.getTFTSerialPort();
		unsigned int brightness = m_conf.getTFTSerialBrightness();

		LogInfo("    Port: %s", port.c_str());
		LogInfo("    Brightness: %u", brightness);

		m_display = new CTFTSerial(callsign, dmrid, port, brightness);
	} else if (type == "Nextion") {
		std::string size        = m_conf.getNextionSize();
		std::string port        = m_conf.getNextionPort();
		unsigned int brightness = m_conf.getNextionBrightness();

		LogInfo("    Size: %s\"", size.c_str());
		LogInfo("    Port: %s", port.c_str());
		LogInfo("    Brightness: %u", brightness);

		m_display = new CNextion(callsign, dmrid, size, port, brightness);
#if defined(HD44780)
	} else if (type == "HD44780") {
		unsigned int rows    = m_conf.getHD44780Rows();
		unsigned int columns = m_conf.getHD44780Columns();
		std::vector<unsigned int> pins = m_conf.getHD44780Pins();
		bool pwm = m_conf.getHD44780PWM();
		unsigned int pwmPin = m_conf.getHD44780PWMPin();
		unsigned int pwmBright = m_conf.getHD44780PWMBright();
		unsigned int pwmDim = m_conf.getHD44780PWMDim();

		if (pins.size() == 6U) {
			LogInfo("    Rows: %u", rows);
			LogInfo("    Columns: %u", columns);
			LogInfo("    Pins: %u,%u,%u,%u,%u,%u", pins.at(0U), pins.at(1U), pins.at(2U), pins.at(3U), pins.at(4U), pins.at(5U));

			if (pwm) {
				LogInfo("PWM Brightness Control Enabled");
				LogInfo("    PWM Pin: %u", pwmPin);
				LogInfo("    PWM Bright: %u", pwmBright);
				LogInfo("    PWM Dim: %u", pwmDim);
			}

			m_display = new CHD44780(rows, columns, callsign, dmrid, pins, pwm, pwmPin, pwmBright, pwmDim, m_duplex);
		}
#endif
	} else {
		m_display = new CNullDisplay;
	}

	bool ret = m_display->open();
	if (!ret) {
		delete m_display;
		m_display = new CNullDisplay;
	}
}

void CMMDVMHost::setMode(unsigned char mode, bool logging)
{
	assert(m_modem != NULL);
	assert(m_display != NULL);

	switch (mode) {
	case MODE_DSTAR:
		if (logging)
			LogMessage("Mode set to D-Star");
		if (m_dmrNetwork != NULL)
			m_dmrNetwork->enable(false);
		m_modem->setMode(MODE_DSTAR);
		m_mode = MODE_DSTAR;
		m_modeTimer.start();
		break;

	case MODE_DMR:
		if (logging)
			LogMessage("Mode set to DMR");
		if (m_dstarNetwork != NULL)
			m_dstarNetwork->enable(false);
		m_modem->setMode(MODE_DMR);
		if (m_duplex) {
			m_modem->writeDMRStart(true);
			m_dmrTXTimer.start();
		}
		m_mode = MODE_DMR;
		m_modeTimer.start();
		break;

	case MODE_YSF:
		if (logging)
			LogMessage("Mode set to System Fusion");
		if (m_dstarNetwork != NULL)
			m_dstarNetwork->enable(false);
		if (m_dmrNetwork != NULL)
			m_dmrNetwork->enable(false);
		m_modem->setMode(MODE_YSF);
		m_mode = MODE_YSF;
		m_modeTimer.start();
		break;

	case MODE_LOCKOUT:
		if (logging)
			LogMessage("Mode set to Lockout");
		if (m_dstarNetwork != NULL)
			m_dstarNetwork->enable(false);
		if (m_dmrNetwork != NULL)
			m_dmrNetwork->enable(false);
		if (m_mode == MODE_DMR && m_duplex && m_modem->hasTX()) {
			m_modem->writeDMRStart(false);
			m_dmrTXTimer.stop();
		}
		m_modem->setMode(MODE_IDLE);
		m_display->setLockout();
		m_mode = MODE_LOCKOUT;
		m_modeTimer.stop();
		break;

	case MODE_ERROR:
		if (logging)
			LogMessage("Mode set to Error");
		if (m_dstarNetwork != NULL)
			m_dstarNetwork->enable(false);
		if (m_dmrNetwork != NULL)
			m_dmrNetwork->enable(false);
		if (m_mode == MODE_DMR && m_duplex && m_modem->hasTX()) {
			m_modem->writeDMRStart(false);
			m_dmrTXTimer.stop();
		}
		m_display->setError("MODEM");
		m_mode = MODE_ERROR;
		m_modeTimer.stop();
		break;

	default:
		if (logging)
			LogMessage("Mode set to Idle");
		if (m_dstarNetwork != NULL)
			m_dstarNetwork->enable(true);
		if (m_dmrNetwork != NULL)
			m_dmrNetwork->enable(true);
		if (m_mode == MODE_DMR && m_duplex && m_modem->hasTX()) {
			m_modem->writeDMRStart(false);
			m_dmrTXTimer.stop();
		}
		m_modem->setMode(MODE_IDLE);
		m_display->setIdle();
		m_mode = MODE_IDLE;
		m_modeTimer.stop();
		break;
	}
}
