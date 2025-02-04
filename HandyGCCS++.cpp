#include <cpuid.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <getopt.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <string>
#include <initializer_list>
#include <list>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

// https://github.com/spotify/linux/blob/master/include/linux/input.h

// begin basic code

#define SAFE_DELETE(p)       { if(p) { delete (p);     (p) = NULL; } }
#define _countof(array) (sizeof(array) / sizeof(array[0]))

static const char* ws = " \t\n\r\f\v";

// trim from end of string (right)
static inline std::string& rtrim(std::string& s, const char* t = ws)
{
	s.erase(s.find_last_not_of(t) + 1);
	return s;
}

// trim from beginning of string (left)
static inline std::string& ltrim(std::string& s, const char* t = ws)
{
	s.erase(0, s.find_first_not_of(t));
	return s;
}

// trim from both ends of string (right then left)
static inline std::string& trim(std::string& s, const char* t = ws)
{
	return ltrim(rtrim(s, t), t);
}

static int readFileContent(const char* pName, char* pBuf, int len)
{
	FILE* fp = fopen(pName, "rt");

	if (fp)
	{
		fread(pBuf, 1, len, fp);
		fclose(fp);
		for (int i = 0; i < len; i++)
		{
			if (pBuf[i] == '\n' || pBuf[i] == '\r')
			{
				pBuf[i] = 0;
				break;
			}
		}
		return 1;
	}
	return 0;
}

static std::string readExeResult(const char* cmd)
{
	char buffer[128];
	std::string result = "";
	FILE* pipe = popen(cmd, "r");

	if (!pipe) return result;
	try
	{
		while (fgets(buffer, 100, pipe) != NULL) result += buffer;
	}
	catch (...)
	{
	}
	pclose(pipe);
	return result;
}

static std::string get_cpu_vendor()
{
	unsigned int level = 0;
	unsigned int eax = 0;
	char vendor[16] = { 0, };

	__get_cpuid(level, &eax, (unsigned int*)&vendor[0], (unsigned int*)&vendor[4], (unsigned int*)&vendor[8]);
	return vendor;
}

static void sleepMS(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static bool fileExists(std::string path)
{
	struct stat buffer;

	return (stat(path.c_str(), &buffer) == 0);
}

static void setMaxScheduling()
{
	sched_param params = { };
	int policy = 0;
	auto th = pthread_self();	

	pthread_getschedparam(th, &policy, &params);
	params.sched_priority = sched_get_priority_max(policy);
	pthread_setschedparam(th, policy, &params);
}

struct deviceItem
{
	std::string path;
	std::string name;
	std::string phys;
	std::string event;
	int bustype;
	int vendor;
	int product;
};

static std::string EVENT_PATH = "/dev/input/";
static std::string HIDE_PATH = EVENT_PATH + ".hidden/";
static std::string HOME_PATH = "/home";
static std::string USER = "";

static bool steam_ifrunning_deckui(std::string cmd)
{
	char str[500] = { 0, };

	if (readFileContent((HOME_PATH + "/.steam/steam.pid").c_str(), str, 400) >= 0)
	{
		int pid = atoi(str);
		char str2[500] = { 0, };

		snprintf(str, 450, "/proc/%d/cmdline", pid);
		if (readFileContent(str, str2, 400) >= 0)
		{
			for (int i = 0; i < 400; i++)
			{
				if (str2[i] == 0)
				{
					if (str2[i + 1] == 0) break;
					else str2[i] = ' ';
				}
			}
			if (strstr(str2, "-gamepadui"))
			{
				std::string steam_path = HOME_PATH + "/.steam/root/ubuntu12_32/steam";
				std::string run = "su " + USER + " -c '" + steam_path + " -ifrunning " + cmd + "'";

				return system(run.c_str()) >= 0;
			}
		}
	}
	return false;
}

static const char* CHIMERA_LAUNCHER_PATH = "/usr/share/chimera/bin/chimera-web-launcher";
static bool launch_chimera()
{
	if (fileExists(CHIMERA_LAUNCHER_PATH))
	{
		std::string run = "su " + USER + " -c " + CHIMERA_LAUNCHER_PATH;

		return system(run.c_str()) >= 0;
	}
	return false;
}

static void get_user()
{
	int count = 0;

	while (1)
	{
		std::string user = readExeResult("who | awk '{print $1}' | sort | head -1");

		if (!user.empty())
		{
			trim(user);
			USER = user;
			break;
		}
		sleep(1);
        count++;
        if (count > 3)
		{
            USER = "deck";
			break;
		}
	}
	HOME_PATH = "/home/" + USER;	
}

static void restore_hidden()
{
	DIR* dir = opendir(HIDE_PATH.c_str());

	if (dir)
	{
		while (1)
		{
			dirent* ent = readdir(dir);

			if (!ent) break;
			if (ent->d_type != DT_DIR) rename((HIDE_PATH + ent->d_name).c_str(), (EVENT_PATH + ent->d_name).c_str());
		}
		closedir(dir);
	}
}

static bool getDevices(std::list<deviceItem>& devices)
{
	for (int i = 0; i < 100; i++)
	{
		std::string event = "event" + std::to_string(i);

		if (fileExists(EVENT_PATH + event))
		{
			int fd = open((EVENT_PATH + event).c_str(), O_RDONLY | O_NONBLOCK);

			if (fd > 0)
			{
				libevdev* dev = NULL;
				int rc = libevdev_new_from_fd(fd, &dev);

				if (rc < 0)
				{
					close(fd);
					fprintf(stderr, "Failed to init libevdev (%s)\n", strerror(-rc));
					continue;
				}

				const char* name = libevdev_get_name(dev);
				if (!name) continue;

				const char* phys = libevdev_get_phys(dev);
				if (!phys) continue;

				deviceItem item;
				item.path = EVENT_PATH + event;
				item.name = name;
				item.phys = phys;
				item.event = event;
				item.bustype = libevdev_get_id_bustype(dev);
				item.vendor = libevdev_get_id_vendor(dev);
				item.product = libevdev_get_id_product(dev);
				devices.push_back(item);

				libevdev_free(dev);
				close(fd);
			}
		}
	}
	return !devices.empty();
}

static int test_bit(const char* bitmask, int bit)
{
	return bitmask[bit / 8] & (1 << (bit % 8));
}

static bool emit_event(int fd, input_event& event)
{
	if (fd >= 0) return write(fd, &event, sizeof(event)) >= 0;
	return false;
}

static bool emit_event(int fd, int ev_type, int ev_code, int ev_value)
{
	if (fd >= 0)
	{
		input_event event = { 0, };

		gettimeofday(&event.time, 0);
		event.type = ev_type;
		event.code = ev_code;
		event.value = ev_value;
		return write(fd, &event, sizeof(event)) >= 0;
	}
	return false;
}

struct evdev
{
	libevdev* dev;
	std::string org_path;
	std::string new_path;

	evdev(libevdev* dev, std::string org_path = "", std::string new_path = "")
	{
		this->dev = dev;
		this->org_path = org_path;
		this->new_path = new_path;
	}
	~evdev()
	{
		if (!org_path.empty() && !new_path.empty()) rename(new_path.c_str(), org_path.c_str());
		if (dev)
		{
			int fd = libevdev_get_fd(dev);

			if (fd >= 0) close(fd);
			libevdev_grab(dev, LIBEVDEV_UNGRAB);
			libevdev_free(dev);
		}
	}
	int active_keys(std::vector<int>& keys)
	{
		keys.clear();
		if (dev)
		{
			int fd = libevdev_get_fd(dev);

			if (fd >= 0)
			{
				char bytes[(KEY_MAX + 7) / 8] = { 0, };
				int ret = ioctl(fd, EVIOCGKEY(sizeof(bytes)), &bytes);

				if (ret >= 0)
				{
					for (int i = 2; i <= KEY_MAX; i++)
					{
						if (test_bit(bytes, i))
						{
							keys.push_back(i);
							if (keys.size() >= 5) break;
						}
					}
				}
			}
		}
		return (int)keys.size();
	}
	bool emit_event(input_event& event)
	{
		return ::emit_event(libevdev_get_fd(dev), event);
	}
	bool emit_event(int ev_type, int ev_code, int ev_value)
	{
		return ::emit_event(libevdev_get_fd(dev), ev_type, ev_code, ev_value);
	}
};

static evdev* grabDevice(std::string name, std::string phys, bool hidden = false, bool rdwr = false)
{
	std::list<deviceItem> devices;

	if (getDevices(devices))
	{
		for (auto device : devices)
		{
			if (device.name == name && device.phys == phys)
			{
				int flag = rdwr ? O_RDWR : O_RDONLY;
				int fd = open(device.path.c_str(), flag | O_NONBLOCK);

				if (fd > 0)
				{
					libevdev* dev = NULL;
					int rc = libevdev_new_from_fd(fd, &dev);

					if (rc < 0)
					{
						close(fd);
						fprintf(stderr, "Failed to init libevdev (%s)\n", strerror(-rc));
						continue;
					}
					int rr = libevdev_grab(dev, LIBEVDEV_GRAB);
					if (hidden)
					{
						std::string dst_path = HIDE_PATH + device.event;

						rename(device.path.c_str(), dst_path.c_str());
						return new evdev(dev, device.path, dst_path);
					}
					return new evdev(dev);
				}
			}
		}
	}
	return NULL;
}

static void resyncDevice(libevdev* dev)
{
	int rc = -1;

	do
	{
		input_event ev;

		rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
	} while (rc == LIBEVDEV_READ_STATUS_SYNC);
}

static void do_rumble(int fd, int button = 0, int interval = 10, int length = 1000, int delay = 0)
{
	ff_effect effect = { 0, };
	effect.type = FF_RUMBLE;
	effect.id = -1;
	effect.trigger.button = button;
	effect.trigger.interval = interval;
	effect.replay.length = length;
	effect.replay.delay = delay;
	effect.u.rumble.strong_magnitude = 0x0000;
	effect.u.rumble.weak_magnitude = 0xffff;
	int err = ioctl(fd, EVIOCSFF, &effect);

	input_event event = { 0, };
	event.type = EV_FF;
	event.code = effect.id;
	event.value = 1;
	err = write(fd, (const void*)&event, sizeof(event));

	sleepMS(interval);

	event.value = 0;
	err = write(fd, (const void*)&event, sizeof(event));

	err = ioctl(fd, EVIOCRMFF, effect.id);
}

static int g_EV_KEY[] =
{
	KEY_ESC, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB, KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T,
	KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER,
	KEY_LEFTCTRL, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
	KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_LEFTSHIFT, KEY_BACKSLASH,
	KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH,
	KEY_RIGHTSHIFT, KEY_KPASTERISK, KEY_LEFTALT, KEY_SPACE, KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
	KEY_NUMLOCK, KEY_SCROLLLOCK, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPMINUS, KEY_KP4,
	KEY_KP5, KEY_KP6, KEY_KPPLUS, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP0, KEY_KPDOT,
	KEY_ZENKAKUHANKAKU, KEY_102ND, KEY_F11, KEY_F12, KEY_RO, KEY_KATAKANA, KEY_HIRAGANA,
	KEY_HENKAN, KEY_KATAKANAHIRAGANA, KEY_MUHENKAN, KEY_KPJPCOMMA, KEY_KPENTER,
	KEY_RIGHTCTRL, KEY_KPSLASH, KEY_SYSRQ, KEY_RIGHTALT, KEY_HOME, KEY_UP, KEY_PAGEUP,
	KEY_LEFT, KEY_RIGHT, KEY_END, KEY_DOWN, KEY_PAGEDOWN, KEY_INSERT, KEY_DELETE, KEY_MACRO,
	KEY_MUTE, KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_POWER, KEY_KPEQUAL, KEY_KPPLUSMINUS, KEY_PAUSE,
	KEY_KPCOMMA, KEY_HANGUEL, KEY_HANJA, KEY_YEN, KEY_LEFTMETA, KEY_RIGHTMETA, KEY_COMPOSE,
	KEY_STOP, KEY_CALC, KEY_SLEEP, KEY_WAKEUP, KEY_MAIL, KEY_BOOKMARKS, KEY_COMPUTER,
	KEY_BACK, KEY_FORWARD, KEY_NEXTSONG, KEY_PLAYPAUSE, KEY_PREVIOUSSONG, KEY_STOPCD,
	KEY_HOMEPAGE, KEY_REFRESH, KEY_F13, KEY_F14, KEY_F15, KEY_SEARCH, KEY_MEDIA,
	BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
	BTN_MODE, BTN_THUMBL, BTN_THUMBR,
	BTN_TRIGGER_HAPPY1, BTN_TRIGGER_HAPPY2,
	BTN_LEFT, BTN_RIGHT, BTN_MIDDLE,
};

struct EV_ABS_STRUCT
{
	int type;
	input_absinfo info;
};

static int g_EV_REL[] = 
{
	REL_X, REL_Y
};

static EV_ABS_STRUCT g_EV_ABS[] =
{
	{ ABS_X, {0, -32768, 32767, 16, 128, 0} },
	{ ABS_Y, {0, -32768, 32767, 16, 128, 0} },
	{ ABS_Z, {0, 0, 255, 0, 0, 0} },
	{ ABS_RX, {0, -32768, 32767, 16, 128, 0} },
	{ ABS_RY, {0, -32768, 32767, 16, 128, 0} },
	{ ABS_RZ, {0, 0, 255, 0, 0, 0} },
	{ ABS_HAT0X, {0, -1, 1, 0, 0, 0} },
	{ ABS_HAT0Y, {0, -1, 1, 0, 0, 0} }
};

static int g_EV_MSC[] =
{
	MSC_SCAN
};

static int g_EV_LED[] =
{
	LED_NUML,
	LED_CAPSL,
	LED_SCROLLL
};

static int g_EV_FF[] =
{
	FF_RUMBLE,
	FF_PERIODIC,
	FF_SQUARE,
	FF_TRIANGLE,
	FF_SINE,
	FF_GAIN
};

/*
#include <libudev.h>

static char *suinput_get_uinput_path()
{
	int orig_errno;

	udev *udev;
	if ((udev = udev_new()) == NULL) return NULL;

	udev_device *udev_dev = udev_device_new_from_subsystem_sysname(udev, "misc", "uinput");
	if (udev_dev == NULL) goto out;

	const char *devnode;
	if ((devnode = udev_device_get_devnode(udev_dev)) == NULL) goto out;

	char *retval = NULL;
	if ((retval = (char *)malloc(strlen(devnode) + 1)) == NULL) goto out;

	strcpy(retval, devnode);
out:
	orig_errno = errno;
	udev_device_unref(udev_dev);
	udev_unref(udev);
	errno = orig_errno;
	return retval;
}
*/

// https://github.com/ev3dev/python-evdev/blob/ev3dev-stretch/evdev/device.py
// https://github.com/tuomasjjrasanen/python-uinput/blob/master/libsuinput/src/suinput.c
struct uinput
{
	int fd;

	uinput()
	{
		//		char *pinput = suinput_get_uinput_path();
		fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
	}
	~uinput()
	{
		if (fd >= 0)
		{
			ioctl(fd, UI_DEV_DESTROY);
			close(fd);
		}
	}
	int EnableEvent(uint16_t ev_type, uint16_t ev_code)
	{
		if (fd >= 0)
		{
			unsigned long io;

			if (ioctl(fd, UI_SET_EVBIT, ev_type) == -1) return -1;

			switch (ev_type)
			{
			case EV_KEY:
				io = UI_SET_KEYBIT;
				break;
			case EV_REL:
				io = UI_SET_RELBIT;
				break;
			case EV_ABS:
				io = UI_SET_ABSBIT;
				break;
			case EV_MSC:
				io = UI_SET_MSCBIT;
				break;
			case EV_SW:
				io = UI_SET_SWBIT;
				break;
			case EV_LED:
				io = UI_SET_LEDBIT;
				break;
			case EV_SND:
				io = UI_SET_SNDBIT;
				break;
			case EV_FF:
				io = UI_SET_FFBIT;
				break;
			default:
				errno = EINVAL;
				return -1;
			}

			return ioctl(fd, io, ev_code);
		}
		return -1;
	}
	bool Create(const char* name = "Handheld Controller", int bustype = BUS_USB, int vendor = 0x045e, int product = 0x028e, int version = 0x110)
	{
		if (fd >= 0)
		{
			uinput_user_dev uidev = { 0, };

			strncpy(uidev.name, name, UINPUT_MAX_NAME_SIZE);
			uidev.id.bustype = bustype;
			uidev.id.vendor = vendor;
			uidev.id.product = product;
			uidev.id.version = version;
			uidev.ff_effects_max = 16;
			for (size_t i = 0; i < _countof(g_EV_KEY); i++) EnableEvent(EV_KEY, g_EV_KEY[i]);
			for (size_t i = 0; i < _countof(g_EV_REL); i++) EnableEvent(EV_REL, g_EV_REL[i]);
			for (size_t i = 0; i < _countof(g_EV_ABS); i++)
			{
				EnableEvent(EV_ABS, g_EV_ABS[i].type);
				uidev.absmin[g_EV_ABS[i].type] = g_EV_ABS[i].info.minimum;
				uidev.absmax[g_EV_ABS[i].type] = g_EV_ABS[i].info.maximum;
				uidev.absfuzz[g_EV_ABS[i].type] = g_EV_ABS[i].info.fuzz;
				uidev.absflat[g_EV_ABS[i].type] = g_EV_ABS[i].info.flat;
			}
			for (size_t i = 0; i < _countof(g_EV_MSC); i++) EnableEvent(EV_MSC, g_EV_MSC[i]);
			for (size_t i = 0; i < _countof(g_EV_LED); i++) EnableEvent(EV_LED, g_EV_LED[i]);
			for (size_t i = 0; i < _countof(g_EV_FF); i++) EnableEvent(EV_FF, g_EV_FF[i]);
			if (write(fd, &uidev, sizeof(uidev)) >= 0 && ioctl(fd, UI_DEV_CREATE) >= 0) return true;
		}
		return false;
	}
	int Read(input_event* pEvent)
	{
		if (fd >= 0)
		{
			pollfd pfd = { 0, };

			pfd.fd = fd;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 100) >= 0)
			{
				int rc = read(fd, pEvent, sizeof(*pEvent));

				return rc;
			}
			return -EAGAIN;
		}
		return -1;
	}
	bool emit_event(input_event& event)
	{
		return ::emit_event(fd, event);
	}
	bool emit_event(int ev_type, int ev_code, int ev_value)
	{
		return ::emit_event(fd, ev_type, ev_code, ev_value);
	}
};

// end basic code

static FILE* g_logStream = NULL;

// begin event command

struct EventCode
{
	int type;
	int code;
	const char* cmd;
};

static const char* OpenChimera = "Open Chimera";
static const char* ToggleGyro = "Toggle Gyro";
static const char* ToggleMouseMode = "Toggle Mouse Mode";
static const char* Toggle_Performance = "Toggle Performance";

static const EventCode EVENT_NULL[] = { {0} };
static const EventCode EVENT_ALT_TAB[] = { {EV_KEY, KEY_LEFTALT}, {EV_KEY, KEY_TAB}, {0} };
static const EventCode EVENT_ESC[] = { {EV_MSC, MSC_SCAN}, {EV_KEY, KEY_ESC}, {0} };
static const EventCode EVENT_KILL[] = { {EV_KEY, KEY_LEFTMETA}, {EV_KEY, KEY_LEFTCTRL}, {EV_KEY, KEY_ESC}, {0} };
static const EventCode EVENT_MODE[] = { {EV_KEY, BTN_MODE}, {0} };
static const EventCode EVENT_OPEN_CHIM[] = { {0, 0, OpenChimera}, {0} };
static const EventCode EVENT_OSK[] = { {EV_KEY, BTN_MODE}, {EV_KEY, BTN_NORTH}, {0} };
static const EventCode EVENT_OSK_DE[] = { {EV_KEY, KEY_LEFTMETA}, {EV_KEY, KEY_LEFTCTRL}, {EV_KEY, KEY_O}, {0} };
static const EventCode EVENT_OSK_NES[] = { {EV_KEY, BTN_MODE}, {EV_KEY, BTN_WEST}, {0} };
static const EventCode EVENT_QAM[] = { {EV_KEY, BTN_MODE}, {EV_KEY, BTN_SOUTH}, {0} };
static const EventCode EVENT_QAM_NES[] = { {EV_KEY, BTN_MODE}, {EV_KEY, BTN_EAST}, {0} };
static const EventCode EVENT_SCR[] = { {EV_KEY, BTN_MODE}, {EV_KEY, BTN_TR}, {0} };
static const EventCode EVENT_THUMBL[] = { {EV_KEY, BTN_THUMBL}, {0} };
static const EventCode EVENT_THUMBR[] = { {EV_KEY, BTN_THUMBR}, {0} };
static const EventCode EVENT_TOGGLE_GYRO[] = { {0, 0, ToggleGyro}, {0} };
static const EventCode EVENT_TOGGLE_MOUSE[] = { {0, 0, ToggleMouseMode}, {0} };
static const EventCode EVENT_TOGGLE_PERF[] = { {0, 0, Toggle_Performance}, {0} };
static const EventCode EVENT_VOLUP[] = { {EV_KEY, KEY_VOLUMEUP}, {0} };
static const EventCode EVENT_VOLDOWN[] = { {EV_KEY, KEY_VOLUMEDOWN}, {0} };

static int getEventCount(const EventCode* pEvent)
{
	int ret = 0;

	while (pEvent)
	{
		if (pEvent->type == 0 && pEvent->code == 0 && pEvent->cmd == NULL) break;
		pEvent++;
		ret++;
	}
	return ret;
}

static std::map<std::string, const EventCode*> EVENT_MAP =
{
	{ "ALT_TAB", EVENT_ALT_TAB },
	{ "ESC", EVENT_ESC },
	{ "KILL", EVENT_KILL },
	{ "MODE", EVENT_MODE },
	{ "OPEN_CHIMERA", EVENT_OPEN_CHIM },
	{ "OSK", EVENT_OSK },
	{ "OSK_DE", EVENT_OSK_DE },
	{ "OSK_NES", EVENT_OSK_NES },
	{ "QAM", EVENT_QAM },
	{ "QAM_NES", EVENT_QAM_NES },
	{ "SCR", EVENT_SCR },
    { "THUMBL", EVENT_THUMBL },
    { "THUMBR", EVENT_THUMBR },
	{ "TOGGLE_GYRO", EVENT_TOGGLE_GYRO },
	{ "TOGGLE_MOUSE", EVENT_TOGGLE_MOUSE },
	{ "TOGGLE_PERFORMANCE", EVENT_TOGGLE_PERF },
	{ "VOLUP", EVENT_VOLUP },
	{ "VOLDOWN", EVENT_VOLDOWN }
};

static const std::vector<const EventCode*> INSTANT_EVENTS =
{ 
	EVENT_MODE, EVENT_OPEN_CHIM, EVENT_TOGGLE_GYRO, EVENT_THUMBL, EVENT_THUMBR, 
	EVENT_TOGGLE_MOUSE, EVENT_TOGGLE_PERF, EVENT_VOLUP, EVENT_VOLDOWN
};
static const std::vector<const EventCode*> QUEUED_EVENTS =
{
	EVENT_ALT_TAB, EVENT_ESC, EVENT_KILL, EVENT_OSK, EVENT_OSK_DE,
	EVENT_OSK_NES, EVENT_QAM, EVENT_QAM_NES, EVENT_SCR
};

static const char* POWER_ACTION_HIBERNATE = "Hibernate";
static const char* POWER_ACTION_SHUTDOWN = "Shutdown";
static const char* POWER_ACTION_SUSPEND = "Suspend";
static const char* POWER_ACTION_SUSPEND_THEN_HIBERNATE = "Suspend then hibernate";
static std::map<std::string, const char*> POWER_ACTION_MAP =
{
	{ "HIBERNATE", 				POWER_ACTION_HIBERNATE },
	{ "SHUTDOWN",  				POWER_ACTION_SHUTDOWN },
	{ "SUSPEND",   				POWER_ACTION_SUSPEND },
	{ "SUSPEND_THEN_HIBERNATE",	POWER_ACTION_SUSPEND_THEN_HIBERNATE },
};

static std::map<std::string, std::string> g_config;

static std::map<int, const EventCode*> g_button_map;
static const char* g_power_action = POWER_ACTION_SUSPEND;
static const char* g_lid_action = POWER_ACTION_SUSPEND;

static void map_config(std::string key, int idx, int power = 0)
{
	auto it1 = g_config.find(key);

	if (it1 == g_config.end())
	{
		if (power == 2) g_lid_action = NULL;
		else if (power) g_power_action = NULL;
		else g_button_map[idx] = EVENT_NULL;
	}
	else
	{
		if (power)
		{
			auto it2 = POWER_ACTION_MAP.find(it1->second);

			if (it2 == POWER_ACTION_MAP.end())
			{
				if (power == 2) g_lid_action = NULL;
				else g_power_action = NULL;
			}
			else
			{
				if (power == 2) g_lid_action = it2->second;
				else g_power_action = it2->second;
			}
		}
		else
		{
			auto it2 = EVENT_MAP.find(it1->second);

			if (it2 == EVENT_MAP.end()) g_button_map[idx] = EVENT_NULL;
			else g_button_map[idx] = it2->second;
		}
	}
}

static void map_config()
{
	map_config("button1", 0);
	map_config("button2", 1);
	map_config("button3", 2);
	map_config("button4", 3);
	map_config("button5", 4);
	map_config("button6", 5);
	map_config("button7", 6);
	map_config("button8", 7);
	map_config("button9", 8);
	map_config("power_button", 0, 1);
	map_config("lid_switch", 0, 2);

	g_button_map[13] = EVENT_VOLUP;
	g_button_map[14] = EVENT_VOLDOWN;
}

static void set_default_config()
{
	g_config["button1"] = "SCR";
	g_config["button2"] = "QAM";
	g_config["button3"] = "ESC";
	g_config["button4"] = "OSK";
	g_config["button5"] = "MODE";
	g_config["button6"] = "OPEN_CHIMERA";
	g_config["button7"] = "TOGGLE_PERFORMANCE";
	g_config["button8"] = "THUMBL";
	g_config["button9"] = "THUMBR";
	g_config["power_button"] = "SUSPEND";
	g_config["lid_switch"] = "SUSPEND";
	map_config();
}

static std::vector<int> g_button_key[2][14] = { };
static void assignButtonKey(int idx, std::initializer_list<int> keys, int other = 0)
{
	if (idx >= 1 && idx <= 14) g_button_key[other][idx - 1] = keys;
	else fprintf(g_logStream, "out of button range: %d \n", idx);
}

static void do_handle_power_action(const char* pAction)
{
	if (pAction) fprintf(g_logStream, "Power Action: %s \n", pAction);
	else fprintf(g_logStream, "Power Action: none \n");
	if (pAction == POWER_ACTION_SUSPEND)
	{
		if (!steam_ifrunning_deckui("steam://shortpowerpress")) system("systemctl suspend");
	}
	else if (pAction == POWER_ACTION_HIBERNATE)
	{
		system("systemctl hibernate");
	}
	else if (pAction == POWER_ACTION_SHUTDOWN)
	{
		if (!steam_ifrunning_deckui("steam://longpowerpress")) system("systemctl poweroff");
	}
	else if (pAction == POWER_ACTION_SUSPEND_THEN_HIBERNATE)
	{
		system("systemctl suspend-then-hibernate");
	}
}

static int g_runningLoop = 1;
const char* g_pPowerAction = NULL;
static void handle_power_action(const char* pAction)
{
	g_pPowerAction = pAction;
	g_runningLoop = 2;
}

static const char* CONFIG_DIR = "/etc/handygccs/";
static const char* CONFIG_PATH = "/etc/handygccs/handygccs.conf";

static void write_config()
{
	FILE* fp = fopen(CONFIG_PATH, "wt");

	if (fp)
	{
		fputs("[Button Map]\r\n", fp);
		fputs("version = 1.2\r\n", fp);
		for (auto& item : g_config)
		{
			fputs((item.first + " = " + item.second + "\r\n").c_str(), fp);
		}
		fclose(fp);
	}
}

static void get_config()
{
	FILE* fp = fopen(CONFIG_PATH, "rt");

	if (fp)
	{
		while (feof(fp) == 0)
		{
			char line[100] = { 0, };

			fgets(line, 90, fp);
			std::string str = line;
			str.erase(std::remove(str.begin(), str.end(), '\r'), str.end());
			str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());

			auto p = str.find("=");
			if (p != std::string::npos)
			{
				std::string left = str.substr(0, p);
				std::string right = str.substr(p + 1, 90);

				trim(left);
				trim(right);
				g_config[left] = right;
			}
		}
		fclose(fp);
		map_config();
	}
	else write_config();
}

// end event command

static uinput* g_ui_device = NULL;

static void handle_signal(int sig)
{
	g_runningLoop = 0;
}

static double  		BUTTON_DELAY = 0.00;
static bool			CAPTURE_CONTROLLER = false;
static bool			CAPTURE_KEYBOARD = false;
static bool    		CAPTURE_POWER = false;
static bool			ROG_ALLY_DEVICE = false;
static std::string	GAMEPAD_ADDRESS = "";
static std::string	GAMEPAD_NAME = "";
static std::string	KEYBOARD_ADDRESS = "";
static std::string	KEYBOARD_NAME = "";
static std::string	KEYBOARD_2_ADDRESS = "";
static std::string	KEYBOARD_2_NAME = "";
static std::string	POWER_BUTTON_PRIMARY = "LNXPWRBN/button/input0";
static std::string	POWER_BUTTON_SECONDARY = "PNP0C0C/button/input0";
static std::string	LID_SWITCH = "";

static int DETECT_DELAY = 500;

static bool id_system(std::string model, std::string board, std::list<deviceItem>& devices)
{
	bool ret = true;
	std::string vendor = get_cpu_vendor();
	std::string AOKZOE_TOGGLE_TURBO = "/sys/devices/platform/oxp-platform/tt_toggle";

	// ASUS Devices
	if (model == "ROG Ally RC71L" || model == "ROG Ally RC71L_RC71L")
	{
		ROG_ALLY_DEVICE = true;
		BUTTON_DELAY = 0.2;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_NAME = "Asus Keyboard";
		KEYBOARD_2_NAME = "Asus Keyboard";

		const char* GAMEPAD_ADDRESS_LIST[] =
		{
			"usb-0000:08:00.3-2/input0",
			"usb-0000:09:00.3-2/input0",
			"usb-0000:0a:00.3-2/input0",
		};
		const char* KEYBOARD_ADDRESS_LIST[] =
		{
			"usb-0000:08:00.3-3/input0",
			"usb-0000:09:00.3-3/input0",
			"usb-0000:0a:00.3-3/input0",
		};
		const char* KEYBOARD_2_ADDRESS_LIST[] =
		{
			"usb-0000:08:00.3-3/input2",
			"usb-0000:09:00.3-3/input2",
			"usb-0000:0a:00.3-3/input2",
		};
		for (auto device : devices)
		{
			for (int i = 0; i < _countof(GAMEPAD_ADDRESS_LIST); i++)
			{
				if (device.phys == GAMEPAD_ADDRESS_LIST[i]) GAMEPAD_ADDRESS = device.phys;
				if (device.phys == KEYBOARD_ADDRESS_LIST[i]) KEYBOARD_ADDRESS = device.phys;
				if (device.phys == KEYBOARD_2_ADDRESS_LIST[i]) KEYBOARD_2_ADDRESS = device.phys;
			}
		}

		// BUTTON 2 (Default: QAM) Armory Crate Button Short Press
		assignButtonKey(2, { 148 });

		// BUTTON 4 (Default: OSK) Control Center Long Press.
		assignButtonKey(4, { 29, 56, 111 });

		// BUTTON 5 (Default: Mode) Control Center Short Press.
		assignButtonKey(5, { 186 });

		// BUTTON 11 (Default: Happy Trigger 1) Left Paddle
		assignButtonKey(11, { 184 });

		// BUTTON 4 (Default: Happy Trigger 2) Right Paddle
		assignButtonKey(12, { 185 });
	}
	// ANBERNIC Devices
	else if (model == "Win600")
	{
		BUTTON_DELAY = 0.04;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:02:00.3-5/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (BUTTON 4 ALT Mode) (Default: Screenshot) Long press KB
		assignButtonKey(1, { 24, 29, 125 });

		// BUTTON 2 (Default: QAM) Home key.
		assignButtonKey(2, { 125 });

		// BUTTON 3, BUTTON 2 ALt mode (Defalt ESC)
		assignButtonKey(3, { 1 });

		// BUTTON 4 (Default: OSK) Short press KB
		assignButtonKey(4, { 24, 29, 125 });

		// BUTTON 5 (Default: GUIDE) Meta/Windows key.
		assignButtonKey(5, { 34, 125 });
	}
	// AOKZOE Devices
	else if (model == "AOKZOE A1 AR07")
	{
		if (fileExists(AOKZOE_TOGGLE_TURBO.c_str()))
		{
			std::string run = "echo 1 > " + AOKZOE_TOGGLE_TURBO;

			system(run.c_str());
		}

		BUTTON_DELAY = 0.09;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:e4:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Possible dangerous fan activity!) Short press orange + |||||
		assignButtonKey(1, { 99, 125 });

		// BUTTON 2 (Default: QAM) Turbo Button
		// [34, 125]  ??
		assignButtonKey(2, { 29, 56, 125 });

		// BUTTON 3 (Default: ESC) Short press orange + KB
		assignButtonKey(3, { 97, 100, 111 });

		// BUTTON 4 (Default: OSK) Short press KB
		assignButtonKey(4, { 24, 97, 125 });

		// BUTTON 5 (Default: MODE) Short press orange
		assignButtonKey(5, { 32, 125 });

		// BUTTON 6 (Default: Launch Chimera) Long press orange
		assignButtonKey(6, { 34, 125 });
	}
	else if (model == "AOKZOE A1 Pro")
	{
		if (fileExists(AOKZOE_TOGGLE_TURBO.c_str()))
		{
			std::string run = "echo 1 > " + AOKZOE_TOGGLE_TURBO;

			system(run.c_str());
		}

		BUTTON_DELAY = 0.09;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:c4:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Possible dangerous fan activity!) Short press orange + |||||
		assignButtonKey(1, { 99, 125 });

		// BUTTON 2 (Default: QAM) Turbo Button
		// [34, 125]  ??
		assignButtonKey(2, { 29, 56, 125 });

		// BUTTON 3 (Default: ESC) Short press orange + KB
		assignButtonKey(3, { 97, 100, 111 });

		// BUTTON 4 (Default: OSK) Short press KB
		assignButtonKey(4, { 24, 97, 125 });

		// BUTTON 5 (Default: MODE) Short press orange
		assignButtonKey(5, { 32, 125 });

		// BUTTON 6 (Default: Launch Chimera) Long press orange
		assignButtonKey(6, { 34, 125 });
	}
	// Aya Neo Devices
	else if (model == "AYA NEO FOUNDER" || model == "AYA NEO 2021" || model == "AYANEO 2021" ||
		model == "AYANEO 2021 Pro" || model == "AYANEO 2021 Pro Retro Power")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:03:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Default: Screenshot) WIN button
		assignButtonKey(1, { 125 });

		// BUTTON 2 (Default: QAM) TM Button
		assignButtonKey(2, { 97, 100, 111 });

		// BUTTON 3 (Default: ESC) ESC Button
		assignButtonKey(3, { 1 });

		// BUTTON 4 (Default: OSK) KB Button
		assignButtonKey(4, { 24, 97, 125 });
	}
	else if (model == "NEXT" || model == "NEXT Pro" || model == "NEXT Advance" ||
		model == "AYANEO NEXT" || model == "AYANEO NEXT Pro" || model == "AYANEO NEXT Advance")
	{
		BUTTON_DELAY = 0.10;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:03:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 2 (Default: QAM) Small Button
		assignButtonKey(2, { 40, 133 });
		assignButtonKey(2, { 32, 125 }, 1);

		// BUTTON 5 (Default: MODE) Big button
		assignButtonKey(5, { 96, 105, 133 });
		assignButtonKey(5, { 88, 97, 125 }, 1);
	}
	else if (model == "AIR" || model == "AIR Pro")
	{
		BUTTON_DELAY = 0.10;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:04:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Default: Screenshot/Launch Chiumera) LC Button
		assignButtonKey(1, { 87, 97, 125 });

		// BUTTON 2 (Default: QAM) Small Button
		assignButtonKey(2, { 32, 125 });

		// BUTTON 4 (Default: OSK) RC Button
		assignButtonKey(4, { 368, 97, 125 });

		// BUTTON 5 (Default: MODE) Big button
		assignButtonKey(5, { 88, 97, 125 });
	}
	else if (model == "AYANEO 2" || model == "GEEK" || model == "AYANEO 2S" || model == "GEEK 1S")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:e4:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Default: Screenshot/Launch Chiumera) LC Button
		assignButtonKey(1, { 97, 125, 185 });

		// BUTTON 2 (Default: QAM) Small Button
		assignButtonKey(2, { 32, 125 });

		// BUTTON 4 (Default: OSK) RC Button
		assignButtonKey(4, { 97, 125, 186 });

		// BUTTON 5 (Default: MODE) Big button
		assignButtonKey(5, { 97, 125, 187 });
	}
	else if (model == "AIR Plus" && board == "AB05-Mendocino")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:05:00.0-1/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Default: Screenshot/Launch Chiumera) LC Button
		assignButtonKey(1, { 97, 125, 185 });

		// BUTTON 2 (Default: QAM) Small Button
		assignButtonKey(2, { 32, 125 });

		// BUTTON 4 (Default: OSK) RC Button
		assignButtonKey(4, { 97, 125, 186 });

		// BUTTON 5 (Default: MODE) Big button
		assignButtonKey(5, { 97, 125, 187 });
	}
	else if (model == "AIR Plus" || model == "SLIDE")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";		
		const char* GAMEPAD_ADDRESS_LIST[] =
		{
			"usb-0000:00:14.0-6/input0", // intel plus
			"usb-0000:64:00.3-3/input0", // amd plus
			"usb-0000:c4:00.3-3/input0", // slider			
		};
		for (auto device : devices)
		{
			for (int i = 0; i < _countof(GAMEPAD_ADDRESS_LIST); i++)
			{
				if (device.phys == GAMEPAD_ADDRESS_LIST[i]) GAMEPAD_ADDRESS = device.phys;
			}
		}

		// BUTTON 1 (Default: Screenshot/Launch Chiumera) LC Button
		assignButtonKey(1, { 29, 125, 185 });

		// BUTTON 2 (Default: QAM) Small Button
		assignButtonKey(2, { 32, 125 });

		// BUTTON 4 (Default: OSK) RC Button
		assignButtonKey(4, { 29, 125, 186 });

		// BUTTON 5 (Default: MODE) Big button
		assignButtonKey(5, { 29, 125, 187 });
	}
	else if (model == "AYANEO 2S" || model == "FLIP KB" || model == "FLIP DS" || model == "GEEK 1S" || model == "AIR 1S" || model == "AIR 1S Limited")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:c4:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Default: Screenshot/Launch Chiumera) LC Button
		assignButtonKey(1, { 97, 125, 185 });

		// BUTTON 2 (Default: QAM) Small Button
		assignButtonKey(2, { 32, 125 });

		// BUTTON 4 (Default: OSK) RC Button
		assignButtonKey(4, { 97, 125, 186 });

		// BUTTON 5 (Default: MODE) Big button
		assignButtonKey(5, { 97, 125, 187 });
	}
	else if (model == "KUN")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:c4:00.3-4.1/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1: LC Button
		assignButtonKey(1, { 97, 125, 185 });

		// BUTTON 2: AYA small Button
		assignButtonKey(2, { 32, 125 });

		// BUTTON 4: RC Button
		assignButtonKey(4, { 97, 125, 186 });

		// BUTTON 5: AYAspace 
		assignButtonKey(5, { 97, 125, 187 });

		// BUTTON 6: T Button
		assignButtonKey(6, { 97, 125, 188 });
	}
	// Ayn Devices
	else if (model == "Loki Max")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:74:00.0-1/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Default: Screenshot) Front lower-left + front lower-right
		assignButtonKey(1, { 111 });

		// BUTTON 2 (Default: QAM) Front lower-right
		assignButtonKey(2, { 20, 29, 42, 56 });
	}
	else if (model == "Loki Zero")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:04:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Default: Screenshot) Front lower-left + front lower-right
		assignButtonKey(1, { 111 });

		// BUTTON 2 (Default: QAM) Front lower-right
		assignButtonKey(2, { 20, 29, 42, 56 });
	}
	else if (model == "Loki MiniPro")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:04:00.4-2/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Default: Screenshot) Front lower-left + front lower-right
		assignButtonKey(1, { 111 });

		// BUTTON 2 (Default: QAM) Front lower-right
		assignButtonKey(2, { 20, 29, 42, 56 });
	}
	// Lenovo Devices
	else if (model == "83E1") // Legion Go
	{
		BUTTON_DELAY = 0.2;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:c2:00.3-3/input0";
		GAMEPAD_NAME = "Generic X-Box pad";
		KEYBOARD_ADDRESS = "usb-0000:c2:00.3-3/input3";
		KEYBOARD_NAME = "  Legion Controller for Windows  Keyboard";

		// Legion + a = QAM
		assignButtonKey(2, { 29, 56, 111 });

		// Legion + x = keyboard
		assignButtonKey(4, { 99 });

		// Legion + B = MODE
		assignButtonKey(5, { 24, 29, 125 });
	}
	// GPD Devices
	// Have 2 buttons with 3 modes (left, right, both)
	else if (model == "G1618-03") // Win3
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:00:14.0-7/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "usb-0000:00:14.0-5/input0";
		KEYBOARD_NAME = "  Mouse for Windows";

		// BUTTON 1 (Default: Screenshot)
		assignButtonKey(1, { 29, 56, 111 });

		// BUTTON 2 (Default: QAM)
		assignButtonKey(2, { 1 });
	}
	else if (model == "G1619-04") // WinMax2
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:74:00.3-3/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "usb-0000:74:00.3-4/input1";
		KEYBOARD_NAME = "  Mouse for Windows";
		//    	KEYBOARD_ADDRESS = "isa0060/serio0/input0";	// for test built-in keyboard
		//    	KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		LID_SWITCH = "PNP0C0D/button/input0";

		// BUTTON 1 (Default: Screenshot)
		assignButtonKey(1, { 11 });
		assignButtonKey(1, { 29, 32 }, 1);

		// BUTTON 2 (Default: QAM)
		assignButtonKey(2, { 10 });
		assignButtonKey(2, { 88 }, 1);
	}
	else if (model == "G1618-04") // Win4
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:73:00.3-4.1/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "usb-0000:73:00.3-4.2/input1";
		KEYBOARD_NAME = "  Mouse for Windows";

		// BUTTON 1 (Default: Screenshot)
		assignButtonKey(1, { 119 });

		// BUTTON 2 (Default: QAM)
		assignButtonKey(2, { 99 });
	}
	else if (model == "G1617-01" || // WinMini 7840U/8840U
		model == "G1617-02") // // WinMini HX370
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:63:00.3-5/input0"; 
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "usb-0000:63:00.3-3/input1";
		KEYBOARD_NAME = "  Mouse for Windows";

		LID_SWITCH = "PNP0C0D/button/input0";

		const char* GAMEPAD_ADDRESS_LIST[] =
		{
			"usb-0000:63:00.3-5/input0",	// 7840U
			"usb-0000:c3:00.3-5/input0",	// 8840U
			"usb-0000:c6:00.0-3.1/input0",	// HX370
		};
		const char* KEYBOARD_ADDRESS_LIST[] =
		{
			"usb-0000:63:00.3-3/input1",	// 7840U	
			"usb-0000:c3:00.3-3/input1",	// 8840U
			"usb-0000:c6:00.0-3.2/input0",	// HX370
		};
		for (auto device : devices)
		{
			for (int i = 0; i < _countof(GAMEPAD_ADDRESS_LIST); i++)
			{
				if (device.phys == GAMEPAD_ADDRESS_LIST[i]) GAMEPAD_ADDRESS = device.phys;
				if (device.phys == KEYBOARD_ADDRESS_LIST[i]) KEYBOARD_ADDRESS = device.phys;
			}
		}

		// BUTTON 1 (Default: Screenshot)
		assignButtonKey(1, { 119 });

		// BUTTON 2 (Default: QAM)
		assignButtonKey(2, { 99 });
		assignButtonKey(2, { 32, 125 }, 1);
	}
	// ONEXPLAYER Devices
	// Older BIOS have incomlete DMI data and most models report as "ONE XPLAYER" or "ONEXPLAYER".
	else if (model == "ONE XPLAYER" || model == "ONEXPLAYER")
	{
		if (vendor.find("GenuineIntel") != std::string::npos)
		{
			BUTTON_DELAY = 0.11;
			CAPTURE_CONTROLLER = true;
			CAPTURE_KEYBOARD = true;
			CAPTURE_POWER = true;
			GAMEPAD_ADDRESS = "usb-0000:00:14.0-9/input0";
			GAMEPAD_NAME = "OneXPlayer Gamepad";
			KEYBOARD_ADDRESS = "isa0060/serio0/input0";
			KEYBOARD_NAME = "AT Translated Set 2 keyboard";

			// BUTTON 1 (Possible dangerous fan activity!) Short press orange + |||||
			assignButtonKey(1, { 99, 125 });

			// BUTTON 2 (Default: QAM) Short press orange
			assignButtonKey(2, { 32, 125 });

			// BUTTON 3 (Default: ESC) Short press orange + KB
			assignButtonKey(3, { 97, 100, 111 });

			// BUTTON 4 (Default: OSK) Short press KB
			assignButtonKey(4, { 24, 97, 125 });
		}
		else
		{
			BUTTON_DELAY = 0.11;
			CAPTURE_CONTROLLER = true;
			CAPTURE_KEYBOARD = true;
			CAPTURE_POWER = true;
			GAMEPAD_ADDRESS = "usb-0000:03:00.3-4/input0";
			GAMEPAD_NAME = "Microsoft X-Box 360 pad";
			KEYBOARD_ADDRESS = "isa0060/serio0/input0";
			KEYBOARD_NAME = "AT Translated Set 2 keyboard";

			// BUTTON 1 (Possible dangerous fan activity!) Short press orange + |||||
			assignButtonKey(1, { 99, 125 });

			// BUTTON 2 (Default: QAM) Long press orange
			assignButtonKey(2, { 34, 125 });

			// BUTTON 3 (Default: ESC) Short press orange + KB
			assignButtonKey(3, { 97, 100, 111 });

			// BUTTON 4 (Default: OSK) Short press KB
			assignButtonKey(4, { 24, 97, 125 });

			// BUTTON 5 (Default: MODE) Short press orange
			assignButtonKey(5, { 32, 125 });
		}
	}
	else if (model == "ONEXPLAYER mini A07")
	{
		BUTTON_DELAY = 0.11;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:03:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Possible dangerous fan activity!) Short press orange + |||||
		assignButtonKey(1, { 99, 125 });

		// BUTTON 2 (Default: QAM) Turbo Button
		assignButtonKey(2, { 29, 56, 125 });

		// BUTTON 3 (Default: ESC) Short press orange + KB
		assignButtonKey(3, { 97, 100, 111 });

		// BUTTON 4 (Default: OSK) Short press KB
		assignButtonKey(4, { 24, 97, 125 });

		// BUTTON 5 (Default: MODE) Short press orange
		assignButtonKey(5, { 32, 125 });

		// BUTTON 6 (Default: Launch Chimera) Long press orange
		assignButtonKey(6, { 34, 125 });
	}
	else if (model == "ONEXPLAYER Mini Pro")
	{
		BUTTON_DELAY = 0.09;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:e3:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 (Possible dangerous fan activity!) Short press orange + |||||
		assignButtonKey(1, { 99, 125 });

		// BUTTON 2 (Default: QAM) Turbo Button
		assignButtonKey(2, { 29, 56, 125 });

		// BUTTON 3 (Default: ESC) Short press orange + KB
		assignButtonKey(3, { 97, 100, 111 });

		// BUTTON 4 (Default: OSK) Short press KB
		assignButtonKey(4, { 24, 97, 125 });

		// BUTTON 5 (Default: MODE) Short press orange
		assignButtonKey(5, { 32, 125 });

		// BUTTON 6 (Default: Launch Chimera) Long press orange
		assignButtonKey(6, { 34, 125 });
	}
	else if (model == "ONEXPLAYER 2 ARP23")
	{
		BUTTON_DELAY = 0.09;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:74:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// Push volume keys for X1/X2 if they are not in volume mode.
		// BUTTON 0 (VOLUP): X1
		assignButtonKey(13, { 32, 125 });

		// BUTTON 00 (VOLDOWN): X2
		assignButtonKey(14, { 24, 29, 125 });

		// BUTTON 2 (Default: QAM) Turbo Button
		assignButtonKey(2, { 29, 56, 125 });
	}
	else if (model == "ONEXPLAYER 2 PRO ARP23P" || model == "ONEXPLAYER 2 PRO ARP23P EVA-01")
	{
		BUTTON_DELAY = 0.09;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:64:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// Push volume keys for X1/X2 if they are not in volume mode.
		// BUTTON 0 (VOLUP): X1
		assignButtonKey(13, { 32, 125 });

		// BUTTON 00 (VOLDOWN): X2
		assignButtonKey(14, { 24, 29, 125 });

		// BUTTON 2 (Default: QAM) Turbo Button
		assignButtonKey(2, { 29, 56, 125 });
	}
	else if (model == "ONEXPLAYER F1")
	{
		BUTTON_DELAY = 0.09;
		CAPTURE_CONTROLLER = true;
		CAPTURE_KEYBOARD = true;
		CAPTURE_POWER = true;
		GAMEPAD_ADDRESS = "usb-0000:c4:00.3-4/input0";
		GAMEPAD_NAME = "Microsoft X-Box 360 pad";
		KEYBOARD_ADDRESS = "isa0060/serio0/input0";
		KEYBOARD_NAME = "AT Translated Set 2 keyboard";

		// BUTTON 1 Short press orange + turbo
		assignButtonKey(1, { 99, 125 });

		// BUTTON 2 (Default: QAM) Turbo Button
		assignButtonKey(2, { 29, 56, 125 });

		// BUTTON 3 (Default: ESC) Short press orange + KB
		assignButtonKey(3, { 97, 100, 111 });

		// BUTTON 4 (Default: OSK) Short press KB
		assignButtonKey(4, { 24, 97, 125 });

		// BUTTON 5 (Default: MODE) Short press orange
		assignButtonKey(5, { 32, 125 });

		// BUTTON 6 (Default: Launch Chimera) Long press orange
		assignButtonKey(6, { 34, 125 });
	}
	else ret = false;

	return ret;
}

static bool keyIsMatch(std::vector<int>& keys1, std::vector<int>& keys2)
{
	if (keys1.size() > 0 && keys1.size() == keys2.size())
	{
		for (auto key1 : keys1)
		{
			if (std::find(keys2.begin(), keys2.end(), key1) == keys2.end()) return false;
		}
		return true;
	}
	return false;
}

static int getMatchButton(std::vector<int>& keys)
{
	for (int ch = 0; ch < 2; ch++)
	{
		for (int btn = 0; btn < 14; btn++)
		{
			if (keyIsMatch(keys, g_button_key[ch][btn])) return btn;
		}
	}
	return -1;
}

static int FF_DELAY = 200;
static int g_controller_fd = -1;

static void do_rumble_effect(bool on)
{
	if (on)
	{
		if (g_controller_fd >= 0) do_rumble(g_controller_fd, 0, 500, 1000, 0);
		sleepMS(FF_DELAY);
	}
	else
	{
		if (g_controller_fd >= 0) do_rumble(g_controller_fd, 0, 100, 1000, 0);
		sleepMS(FF_DELAY);
		if (g_controller_fd >= 0) do_rumble(g_controller_fd, 0, 100, 1000, 0);
		sleepMS(FF_DELAY);
	}
}

static bool g_mouseMode = false;
static void toggle_mouse_mode(bool doRumble = true)
{
	g_mouseMode = !g_mouseMode;
	if (doRumble) do_rumble_effect(g_mouseMode);
}

#define FLAG_KEY_SELECT 	1
#define FLAG_KEY_START 		2

static int g_setMouseMode = 0;
static void set_mouse_mode()
{
	g_setMouseMode = FLAG_KEY_SELECT | FLAG_KEY_START;
	do_rumble_effect(g_setMouseMode);
}

static void reset_mouse_mode(int flag)
{
	if (g_setMouseMode & flag)
	{
		g_setMouseMode &= ~flag;
		if (g_setMouseMode == 0) toggle_mouse_mode(false);
	}
}

static std::thread *g_mouseThread = NULL;
static std::mutex g_mouseMutex;
static std::condition_variable g_mouseCond;

static int getTickCount()
{
	static timespec old;
	static bool init = false;
	if (!init) 
	{		
		clock_gettime(CLOCK_MONOTONIC, &old);
		init = true;
	}

	timespec now;	
	clock_gettime(CLOCK_MONOTONIC, &now);

	long time = 1000000000 * (now.tv_sec - old.tv_sec) + (now.tv_nsec - old.tv_nsec);
	return (int)(time / 1000000);
}

static int g_oldAbsX = 0;
static int g_oldAbsY = 0;
static int g_btnSelectTick = 0;
static int g_btnStartTick = 0;
static bool g_ltDown = false;
static bool g_rtDown = false;
static void sendMouseEvent()
{
	int tickAbsX = 0;
	int tickAbsY = 0;

	while (g_mouseThread && g_runningLoop)
	{
		if (g_btnSelectTick && g_btnStartTick && getTickCount() - g_btnSelectTick > 2000 && getTickCount() - g_btnStartTick > 2000)
		{
			g_btnSelectTick = 0;
			g_btnStartTick = 0;
			set_mouse_mode();
		}
		{
			std::unique_lock<std::mutex> lock(g_mouseMutex);

			g_mouseCond.wait_for(lock, std::chrono::milliseconds(g_mouseMode ? 5 : 30));
		}
		if (g_mouseMode && g_ui_device)
		{
			int oldAbsX = g_oldAbsX;
			int oldAbsY = g_oldAbsY;

			if (g_ltDown)
			{
				tickAbsX = 0;
				tickAbsY = 0;
			}
			if (g_rtDown)
			{
				tickAbsX = 1;
				tickAbsY = 1;
			} 

			// move cursor
			bool sendSync = false;
			if (abs(oldAbsX) > 4000)
			{
				int mul = 5;

				if (tickAbsX == 0) tickAbsX = getTickCount();
				if (getTickCount() - tickAbsX < 200) mul = 1;
				else if (getTickCount() - tickAbsX < 400) mul = 2;
				else if (getTickCount() - tickAbsX < 600) mul = 3;
				else if (getTickCount() - tickAbsX < 800) mul = 4;

				g_ui_device->emit_event(EV_REL, REL_X, (oldAbsX < 0 ? -1 : 1) * mul);
				sendSync = true;
			}
			else tickAbsX = 0;

			if (abs(oldAbsY) > 4000)
			{
				int mul = 5;

				if (tickAbsY == 0) tickAbsY = getTickCount();
				if (getTickCount() - tickAbsY < 200) mul = 1;
				else if (getTickCount() - tickAbsY < 400) mul = 2;
				else if (getTickCount() - tickAbsY < 600) mul = 3;
				else if (getTickCount() - tickAbsY < 800) mul = 4;

				g_ui_device->emit_event(EV_REL, REL_Y, (oldAbsY < 0 ? -1 : 1) * mul);
				sendSync = true;
			}
			else tickAbsY = 0;

			if (sendSync) g_ui_device->emit_event(EV_SYN, SYN_REPORT, 0);
		}
	}
}

static void do_mouse_mode(input_event stickEvent)
{
	if (EV_ABS == stickEvent.type)
	{
		if (!g_mouseThread) g_mouseThread = new std::thread(sendMouseEvent);
		{
			if (ABS_RX == stickEvent.code) g_oldAbsX = stickEvent.value;
			else if (ABS_RY == stickEvent.code) g_oldAbsY = stickEvent.value;
			else if (ABS_RZ == stickEvent.code) g_rtDown = stickEvent.value >= 200; // rt
			else if (ABS_Z == stickEvent.code) g_ltDown = stickEvent.value >= 200; // lt
		}
	}
	else if (EV_KEY == stickEvent.type)
	{
		if ((BTN_TL == stickEvent.code || BTN_TR == stickEvent.code) && g_ui_device) // lb/lr
		{
			g_ui_device->emit_event(EV_KEY, BTN_TL == stickEvent.code ? BTN_LEFT : BTN_RIGHT, stickEvent.value);

			g_ui_device->emit_event(EV_REL, REL_X, 0);

			g_ui_device->emit_event(EV_REL, REL_Y, 0);

			g_ui_device->emit_event(EV_SYN, SYN_REPORT, 0);
		}
	}
}

static std::string g_performance_mode = "--power-saving";
static std::string g_thermal_mode = "0";

static void toggle_performance()
{
	if (g_performance_mode == "--max-performance")
	{
		g_performance_mode = "--power-saving";
		do_rumble_effect(false);
	}
	else
	{
		g_performance_mode = "--max-performance";
		do_rumble_effect(true);
	}

	std::string ryzenadj_command = "ryzenadj " + g_performance_mode;
	std::string run = readExeResult(ryzenadj_command.c_str());
	fprintf(g_logStream, "%s\n.", run.c_str());

	if (ROG_ALLY_DEVICE)
	{
		if (g_thermal_mode == "1") g_thermal_mode = "0";
		else g_thermal_mode = "1";

		std::string command = "echo " + g_thermal_mode + " > /sys/devices/platform/asus-nb-wmi/throttle_thermal_policy";
		run = readExeResult(command.c_str());
		fprintf(g_logStream, "Thermal mode set to %s -> %s\n.", g_thermal_mode.c_str(), run.c_str());
	}
}

static void emit_now(const EventCode* pCode, bool isDown)
{
	if (pCode && pCode != EVENT_NULL)
	{
		if (pCode->cmd) // string cmd
		{
			if (pCode->cmd == OpenChimera)
			{
				fprintf(g_logStream, "Open Chimera\n.");
				launch_chimera();
			}
			else if (pCode->cmd == ToggleGyro)
			{
				fprintf(g_logStream, "Toggle Gyro is not currently enabled\n.");
			}
			else if (pCode->cmd == ToggleMouseMode)
			{
				fprintf(g_logStream, "Toggle Mouse Mode\n.");
				toggle_mouse_mode();
			}
			else if (pCode->cmd == Toggle_Performance)
			{
				fprintf(g_logStream, "Toggle Performance\n.");
				toggle_performance();
			}
			else if (pCode->cmd == POWER_ACTION_HIBERNATE || pCode->cmd == POWER_ACTION_SHUTDOWN || 
				pCode->cmd == POWER_ACTION_SUSPEND || pCode->cmd == POWER_ACTION_SUSPEND_THEN_HIBERNATE)
			{
				fprintf(g_logStream, "Power mode %s set to button action. Check your configuration file.\n", pCode->cmd);
			}
			else
			{
				fprintf(g_logStream, "%s not defined.\n", pCode->cmd);
			}
		}
		else if (g_ui_device)
		{
			int cnt = getEventCount(pCode);

			if (isDown == false) // need reversed
			{
				for (int i = cnt - 1; i >= 0; i--)
				{
					g_ui_device->emit_event(pCode[i].type, pCode[i].code, isDown ? 1 : 0);
					g_ui_device->emit_event(EV_SYN, SYN_REPORT, 0);
					if (i > 0) sleepMS((int)(BUTTON_DELAY * 1000));
				}
			}
			else
			{
				for (int i = 0; i < cnt; i++)
				{
					g_ui_device->emit_event(pCode[i].type, pCode[i].code, isDown ? 1 : 0);
					g_ui_device->emit_event(EV_SYN, SYN_REPORT, 0);
					if (i < cnt - 1) sleepMS((int)(BUTTON_DELAY * 1000));
				}
			}
		}
	}
}

static void handle_key_down(int event)
{
	auto it = g_button_map.find(event);
	if (it != g_button_map.end())
	{
		if (it->second == EVENT_QAM && g_controller_fd >= 0) do_rumble(g_controller_fd, 0, 150, 1000, 0);

		if (std::find(INSTANT_EVENTS.begin(), INSTANT_EVENTS.end(), it->second) != INSTANT_EVENTS.end())
		{
			emit_now(it->second, true);
			sleepMS(100);
			emit_now(it->second, false);
		}
		else if (std::find(QUEUED_EVENTS.begin(), QUEUED_EVENTS.end(), it->second) != QUEUED_EVENTS.end())
		{
			emit_now(it->second, true);
		}
	}
}

static void handle_key_up(int event)
{
	auto it = g_button_map.find(event);
	if (it != g_button_map.end())
	{
		if (std::find(QUEUED_EVENTS.begin(), QUEUED_EVENTS.end(), it->second) != QUEUED_EVENTS.end())
		{
			emit_now(it->second, false);
		}
	}
}

static std::mutex g_event_mutex;
static std::condition_variable g_event_cond;
static std::list<int> g_event_list;
static int g_debug = 0;

static void process_key(evdev* pDev, input_event& seed_event)
{
	if (seed_event.code == KEY_VOLUMEDOWN || seed_event.code == KEY_VOLUMEUP)
	{
		if (g_ui_device)
		{
			g_ui_device->emit_event(seed_event);
			g_ui_device->emit_event(EV_SYN, SYN_REPORT, 0);
		}
		return;
	}

	std::vector<int> active_keys;
	pDev->active_keys(active_keys);
	if (g_debug)
	{
		fprintf(g_logStream, "Type: %d, Code: %d, Value: %d", seed_event.type, seed_event.code, seed_event.value);
		if (!active_keys.empty())
		{
			fprintf(g_logStream, ", [");
			for (int i = 0; i < active_keys.size(); i++)
			{
				if (i == 0) fprintf(g_logStream, "%d", active_keys[i]);
				else fprintf(g_logStream, ", %d", active_keys[i]);
			}
			fprintf(g_logStream, "]");
		}
		fprintf(g_logStream, "\n");
	}

	int btn = getMatchButton(active_keys);
	if (btn >= 0)
	{
		std::unique_lock<std::mutex> lock(g_event_mutex);

		g_event_list.push_back(btn);
		g_event_cond.notify_all();
	}
}

static int readEvent(evdev** ppDev, input_event* pEvent, const char* log)
{
	pollfd pfd = { 0, };

	pfd.fd = libevdev_get_fd((*ppDev)->dev);
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 100) >= 0)
	{
		int rc = libevdev_next_event((*ppDev)->dev, LIBEVDEV_READ_FLAG_NORMAL, pEvent);

		if (rc == LIBEVDEV_READ_STATUS_SYNC) resyncDevice((*ppDev)->dev);
		else if (rc != LIBEVDEV_READ_STATUS_SUCCESS && rc != -EAGAIN) // error
		{
			fprintf(g_logStream, "%s", log);
			sleepMS(DETECT_DELAY);
			SAFE_DELETE(*ppDev);
		}
		return rc;
	}
	return -EAGAIN;
}

static void capture_controller_events()
{
	evdev* controller_device = NULL;

	setMaxScheduling();
	while (g_runningLoop == 1)
	{
		if (controller_device)
		{
			input_event event = { 0, };
			int rc = readEvent(&controller_device, &event, "Error capture_controller_events devices. Restarting.\n");

			if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
			{
				if (event.type == EV_FF || event.type == EV_UINPUT) continue;
				if (g_ui_device)
				{
					if (g_mouseMode) do_mouse_mode(event);
					else 
					{
						g_ui_device->emit_event(event);
						if (event.type != EV_SYN) g_ui_device->emit_event(EV_SYN, SYN_REPORT, 0);
					}
					if (EV_KEY == event.type)
					{
						if (event.code == BTN_SELECT)
						{
							if (event.value == 0)
							{
								g_btnSelectTick = 0;
								reset_mouse_mode(FLAG_KEY_SELECT);
							} 
							else
							{
								if (g_btnSelectTick == 0) g_btnSelectTick = getTickCount();
							}
						}
						else if (event.code == BTN_START)
						{
							if (event.value == 0)
							{
								g_btnStartTick = 0;
								reset_mouse_mode(FLAG_KEY_START);
							}
							else
							{
								if (g_btnStartTick == 0) g_btnStartTick = getTickCount();
							}
						}
					}
					if (g_btnSelectTick && g_btnStartTick)
					{
						if (!g_mouseThread) g_mouseThread = new std::thread(sendMouseEvent);
					}
				}
			}
		}
		else
		{
			if (!GAMEPAD_ADDRESS.empty() && !GAMEPAD_NAME.empty())
			{
				fprintf(g_logStream, "Attempting to grab controller device...'%s' '%s'\n", GAMEPAD_NAME.c_str(), GAMEPAD_ADDRESS.c_str());
				controller_device = grabDevice(GAMEPAD_NAME, GAMEPAD_ADDRESS, true, true);
				g_controller_fd = controller_device ? libevdev_get_fd(controller_device->dev) : -1;
			}
			if (!controller_device) sleepMS(DETECT_DELAY);
		}
	}
	SAFE_DELETE(controller_device);
}

static void capture_ff_events()
{
	std::map<int, bool> ff_effect_id_set;
	int controller_fd = g_controller_fd;

	while (g_runningLoop == 1)
	{
		if (g_ui_device)
		{
			input_event event = { 0, };
			int rc = g_ui_device->Read(&event);

			if (rc == sizeof(event))
			{
				if (controller_fd != g_controller_fd)
				{
					controller_fd = g_controller_fd;
					ff_effect_id_set.clear();
				}
				if (event.type == EV_FF)
				{
					emit_event(controller_fd, event.type, event.code, event.value);
				}
				else if (event.type == EV_UINPUT)
				{
					int fd = g_ui_device->fd;

					if (event.code == UI_FF_UPLOAD)
					{
						uinput_ff_upload upload = { 0, };

						upload.request_id = event.value;
						int err = ioctl(fd, UI_BEGIN_FF_UPLOAD, &upload);
						auto effect = upload.effect;

						if (ff_effect_id_set.find(effect.id) == ff_effect_id_set.end()) effect.id = -1; // set to -1 for kernel to allocate a new id. all other values throw an error for invalid input
						if (controller_fd >= 0) err = ioctl(controller_fd, EVIOCSFF, &effect);
						ff_effect_id_set[effect.id] = true;
						upload.retval = 0;

						err = ioctl(fd, UI_END_FF_UPLOAD, &upload);
					}
					else if (event.code == UI_FF_ERASE)
					{
						uinput_ff_erase erase = { 0, };

						erase.request_id = event.value;
						int err = ioctl(fd, UI_BEGIN_FF_ERASE, &erase);

						if (controller_fd >= 0) err = ioctl(controller_fd, EVIOCRMFF, erase.effect_id);
						erase.retval = 0;

						auto it = ff_effect_id_set.find(erase.effect_id);
						if (it != ff_effect_id_set.end()) ff_effect_id_set.erase(it);

						err = ioctl(fd, UI_END_FF_ERASE, &erase);
					}
				}
			}
		}
	}
}

static void capture_keyboard_events()
{
	evdev* keyboard_device = NULL;

	while (g_runningLoop == 1)
	{
		if (keyboard_device)
		{
			input_event event = { 0, };
			int rc = readEvent(&keyboard_device, &event, "Error capture_keyboard_events devices. Restarting.\n");

			if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
			{
				process_key(keyboard_device, event);
			}
		}
		else
		{
			if (!KEYBOARD_ADDRESS.empty() && !KEYBOARD_NAME.empty())
			{
				fprintf(g_logStream, "Attempting to grab keyboard device... '%s' '%s'\n", KEYBOARD_NAME.c_str(), KEYBOARD_ADDRESS.c_str());
				keyboard_device = grabDevice(KEYBOARD_NAME, KEYBOARD_ADDRESS, true);
			}
			if (!keyboard_device) sleepMS(DETECT_DELAY);
		}
	}
	SAFE_DELETE(keyboard_device);
}

static void capture_keyboard_2_events()
{
	evdev* keyboard_2_device = NULL;

	while (g_runningLoop == 1)
	{
		if (keyboard_2_device)
		{
			input_event event = { 0, };
			int rc = readEvent(&keyboard_2_device, &event, "Error capture_keyboard_2_events devices. Restarting.\n");

			if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
			{
				process_key(keyboard_2_device, event);
			}
		}
		else
		{
			if (!KEYBOARD_2_ADDRESS.empty() && !KEYBOARD_2_NAME.empty())
			{
				fprintf(g_logStream, "Attempting to grab keyboard device 2 '%s' '%s'\n", KEYBOARD_2_NAME.c_str(), KEYBOARD_2_ADDRESS.c_str());
				keyboard_2_device = grabDevice(KEYBOARD_2_NAME, KEYBOARD_2_ADDRESS, true);
			}
			if (!keyboard_2_device) sleepMS(DETECT_DELAY);
		}
	}
	SAFE_DELETE(keyboard_2_device);
}

static void capture_power_events()
{
	evdev* power_device = NULL;
	evdev* power_device_2 = NULL;
	evdev* lid_switch = NULL;

	while (g_runningLoop == 1)
	{
		if (power_device || power_device_2)
		{
			if (power_device)
			{
				input_event event = { 0, };
				int rc = readEvent(&power_device, &event, "Error power_device. Restarting.\n");

				if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
				{
					if (event.type == EV_KEY && event.code == 116 && event.value == 0) handle_power_action(g_power_action);
				}
			}
			if (power_device_2)
			{
				input_event event = { 0, };
				int rc = readEvent(&power_device_2, &event, "Error power_device_2. Restarting.\n");

				if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
				{
					if (event.type == EV_KEY && event.code == 116 && event.value == 0) handle_power_action(g_power_action);
				}
			}
			if (lid_switch)
			{
				input_event event = { 0, };
				int rc = readEvent(&lid_switch, &event, "Error lid_switch. Restarting.\n");

				if (rc == LIBEVDEV_READ_STATUS_SUCCESS)
				{
					if (event.type == EV_SW && event.code == 0 && event.value == 1) handle_power_action(g_lid_action);
				}
			}
		}
		else
		{
			if (!POWER_BUTTON_PRIMARY.empty())
			{
				fprintf(g_logStream, "Attempting to grab primary power device... '%s' \n", POWER_BUTTON_PRIMARY.c_str());
				power_device = grabDevice("Power Button", POWER_BUTTON_PRIMARY);
			}
			if (!POWER_BUTTON_SECONDARY.empty())
			{
				fprintf(g_logStream, "Attempting to grab secondary power device... '%s' \n", POWER_BUTTON_SECONDARY.c_str());
				power_device_2 = grabDevice("Power Button", POWER_BUTTON_SECONDARY);
			}
			if (!LID_SWITCH.empty())
			{
				fprintf(g_logStream, "Attempting to grab lid switch device... '%s' \n", LID_SWITCH.c_str());
				lid_switch = grabDevice("Lid Switch", LID_SWITCH);
			}
			if (!power_device && !power_device_2) sleepMS(DETECT_DELAY);
		}
	}
	SAFE_DELETE(lid_switch);
	SAFE_DELETE(power_device_2);
	SAFE_DELETE(power_device);
}

int main(int argc, char* argv[])
{
	getTickCount();
	g_logStream = stderr;

	signal(SIGINT, handle_signal);
	signal(SIGHUP, handle_signal);
	signal(SIGTERM, handle_signal);

	char model[110] = { 0, };
	readFileContent("/sys/devices/virtual/dmi/id/product_name", model, 100);

	char board[110] = { 0, };
	readFileContent("/sys/devices/virtual/dmi/id/board_name", board, 100);

	if (argc > 1)
	{
		std::list<deviceItem> devices;
		if (getDevices(devices))
		{
			int powerCnt = 0;

			KEYBOARD_NAME = "AT Translated Set 2 keyboard";
			GAMEPAD_NAME = "Microsoft X-Box 360 pad";
			for (auto device : devices)
			{
				fprintf(g_logStream, "----%s----\n", device.path.c_str());
				fprintf(g_logStream, "Input device name: \"%s\"\n", device.name.c_str());
				fprintf(g_logStream, "Input device phys: \"%s\"\n", device.phys.c_str());
				fprintf(g_logStream, "Input device ID: bus %#x vendor %#x product %#x\n\n", device.bustype, device.vendor, device.product);

				if (device.name == "Power Button")
				{
					if (powerCnt == 0) POWER_BUTTON_PRIMARY = device.phys.c_str();
					else POWER_BUTTON_SECONDARY = device.phys.c_str();
					powerCnt++;
				}
				else if (device.name == "Lid Switch") LID_SWITCH = device.phys.c_str();
				else if (device.name == KEYBOARD_NAME) KEYBOARD_ADDRESS = device.phys.c_str();
				else if (device.name == GAMEPAD_NAME) GAMEPAD_ADDRESS = device.phys.c_str();
			}

			fprintf(g_logStream, "Model name is '%s' with debug mode!!\n", model);

			g_debug = 1;
		}
		else
		{
			fprintf(g_logStream, "cannot get device list\n");
			return -1;
		}
	}

	get_user();

	mkdir(HIDE_PATH.c_str(), 0777);
	restore_hidden();

	mkdir(CONFIG_DIR, 0777);
	set_default_config();
	get_config();

	int ret = 0;
	g_ui_device = new uinput();
	if (g_ui_device->Create())
	{
		while (g_runningLoop)
		{
			g_runningLoop = 1;

			std::list<deviceItem> devices;
			if (getDevices(devices))
			{
				if (!id_system(model, board, devices))
				{
					fprintf(g_logStream, "%s is not currently supported by this tool. Open an issue on " \
						"ub at https://github.ShadowBlip/HandyGCCS if this is a bug. If possible, " \
						"se run the capture-system.py utility found on the GitHub repository and upload " \
						"the file with your issue.\n", model);
					ret = -1;
					break;
				}

				std::list<std::thread*> threads;
				if (CAPTURE_CONTROLLER && !GAMEPAD_ADDRESS.empty() && !GAMEPAD_NAME.empty())
				{
					threads.push_back(new std::thread(capture_controller_events));
					threads.push_back(new std::thread(capture_ff_events));
				}
				if (CAPTURE_KEYBOARD)
				{
					if (!KEYBOARD_ADDRESS.empty() && !KEYBOARD_NAME.empty()) threads.push_back(new std::thread(capture_keyboard_events));
					if (!KEYBOARD_2_ADDRESS.empty() && !KEYBOARD_2_NAME.empty()) threads.push_back(new std::thread(capture_keyboard_2_events));
				}
				if (CAPTURE_POWER)
				{
					threads.push_back(new std::thread(capture_power_events));
				}

				int prev_event = -1;
				while (g_runningLoop == 1)
				{
					std::unique_lock<std::mutex> lock(g_event_mutex);

					if (g_event_list.empty()) g_event_cond.wait_for(lock, std::chrono::milliseconds(300));
					if (g_event_list.empty())
					{
						if (prev_event >= 0) handle_key_up(prev_event);
						prev_event = -1;
					}
					else
					{
						int event = g_event_list.front();

						g_event_list.pop_front();
						if (prev_event != event)
						{
							if (prev_event >= 0) handle_key_up(prev_event);
							handle_key_down(event);
							prev_event = event;
						}
					}
				}

				for (auto& thread : threads)
				{
					thread->join();
					SAFE_DELETE(thread);
				}
				threads.clear();
				if (g_pPowerAction)
				{
					do_handle_power_action(g_pPowerAction);
					g_pPowerAction = NULL;
				}
			}
			else
			{
				fprintf(g_logStream, "cannot get device list\n");
				ret = -1;
				break;
			}
		}

		if (g_mouseThread)
		{
			auto old = g_mouseThread;

			g_mouseThread = NULL;
			old->join();
			SAFE_DELETE(old);
		}
	}
	else
	{
		fprintf(g_logStream, "cannot create uinput\n");
		ret = -1;
	}
	SAFE_DELETE(g_ui_device);
	return ret;
}
