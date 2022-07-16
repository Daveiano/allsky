#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <string>
#include <tr1/memory>
#include <stdlib.h>
#include <signal.h>
#include <fstream>
#include <stdarg.h>

#include <iomanip>
#include <cstring>
#include <sstream>

#include "include/allsky_common.h"
#include "include/RPiHQ_raspistill.h"
#include "include/mode_RPiHQ_mean.h"

#define CAMERA_TYPE				"RPi"
#define IS_RPi
#include "ASI_functions.cpp"

using namespace std;

// Define's specific to this camera type.  Others that apply to all camera types are in allsky_common.h
#define DEFAULT_FONTSIZE		32
#define DEFAULT_DAYGAIN			1.0
#define DEFAULT_DAYAUTOGAIN		true
#define DEFAULT_DAYMAXGAIN		16.0
#define DEFAULT_NIGHTGAIN		4.0
#define DEFAULT_NIGHTAUTOGAIN	true
#define DEFAULT_NIGHTMAXGAIN	16.0


#define DEFAULT_DAYWBR			2.5
#define DEFAULT_DAYWBB			2.0
#define DEFAULT_NIGHTWBR		DEFAULT_DAYWBR		// change if people report different values for night
#define DEFAULT_NIGHTWBB		DEFAULT_DAYWBB		// change if people report different values for night

//-------------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------------

// These are global so they can be used by other routines.
// Variables for command-line settings are first.
bool isLibcamera;									// are we using libcamera or raspistill?
long flip					= DEFAULT_FLIP;
bool tty					= false;				// are we on a tty?
bool notificationImages		= DEFAULT_NOTIFICATIONIMAGES;
char const *fileName		= DEFAULT_FILENAME;
char const *timeFormat		= DEFAULT_TIMEFORMAT;
bool dayAutoAWB				= DEFAULT_DAYAUTOAWB;
double dayWBR				= DEFAULT_DAYWBR;
double dayWBB				= DEFAULT_DAYWBB;
bool nightAutoAWB			= DEFAULT_NIGHTAUTOAWB;
double nightWBR				= DEFAULT_NIGHTWBR;
double nightWBB				= DEFAULT_NIGHTWBB;
bool currentAutoAWB			= false;
double currentWBR			= NOT_SET;
double currentWBB			= NOT_SET;
long actualTemp				= -999;				// temp of sensor during last image. -999 means not supported
timeval exposureStartDateTime;						// date/time an image started

std::vector<int> compressionParameters;
bool bMain					= true;
bool bDisplay				= false;
std::string dayOrNight;
bool saveCC					= false;				// Save Camera Capabilities and exit?
char const *CC_saveDir		= DEFAULT_SAVEDIR;		// Where to put camera's capabilities
int numErrors				= 0;					// Number of errors in a row
bool gotSignal				= false;				// did we get a SIGINT (from keyboard), or SIGTERM/SIGHUP (from service)?
int iNumOfCtrl				= NOT_SET;				// Number of camera control capabilities
int CamNum					= 0;					// 1st camera - we don't support multiple cams
pthread_t threadDisplay		= 0;					// Not used by Rpi;
int numExposures			= 0;					// how many valid pictures have we taken so far?
long cameraMinExposure_us	= NOT_SET;				// camera's minimum exposure - camera dependent
long cameraMaxExposure_us	= NOT_SET;				// camera's maximum exposure - camera dependent
long cameraMaxAutoexposure_us = NOT_SET;			// camera's max auto-exposure
double minSaturation;								// produces black and white
double maxSaturation;
double defaultSaturation;
long minBrightness;									// what user enters on command line
long maxBrightness;
long defaultBrightness;
long currentBrightness		= NOT_SET;
int currentBpp				= NOT_SET;				// bytes per pixel: 8, 16, or 24
int currentBitDepth			= NOT_SET;				// 8 or 16
int currentBin				= NOT_SET;
float mean					= NOT_SET;				// mean brightness of image
char finalFileName[200];							// final name of the file that's written to disk, with no directories
char fullFilename[1000];							// full name of file written to disk
bool currentAutoExposure	= false;				// is auto-exposure currently on?
bool currentAutoGain		= false;				// is auto-gain currently on?
bool quietExit				= false;				// Hide message on exit?
raspistillSetting myRaspistillSetting;
modeMeanSetting myModeMeanSetting;

//-------------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------------


char const *getCameraCommand(bool libcamera)
{
	if (libcamera)
		return("libcamera-still");
	else
		return("raspistill");
}

// Build capture command to capture the image from the HQ camera
int RPiHQcapture(bool autoExposure, long exposure_us, long bin, bool autoGain, double gain, bool autoAWB, double WBR, double WBB, long rotation, long flip, double saturation, long brightness, long quality, char const* fileName, int takingDarkFrames, int preview, long width, long height, bool libcamera, cv::Mat *image, char const *imagetype)
{
	// Define command line.
	string command = getCameraCommand(libcamera);

	// Ensure no process is still running.
	string kill = "pgrep '" + command + "' | xargs kill -9 2> /dev/null";
	char kcmd[kill.length() + 1];		// Define char variable
	strcpy(kcmd, kill.c_str());			// Convert command to character variable
	Log(5, " > Kill command: %s\n", kcmd);
	system(kcmd);						// Stop any currently running process

	if (libcamera)
	{
		// Tried putting this in putenv() but it didn't seem to work.
		command = "LIBCAMERA_LOG_LEVELS=ERROR,FATAL " + command;
	}
	stringstream ss;

	ss << fileName;
	command += " --output '" + ss.str() + "'";
	if (libcamera)
	{
		// xxx TODO: does this do anything?
		// command += " --tuning-file /usr/share/libcamera/ipa/raspberrypi/imx477.json";
		command += "";	// xxxx

		if (strcmp(imagetype, "png") == 0)
			command += " --encoding png";
	}
	else
	{
		command += " --thumb none --burst -st";
	}

	// --timeout (in MS) determines how long the video will run before it takes a picture.
	if (preview)
	{
		stringstream wh;
		wh << width << "," << height;
		command += " --timeout 5000";
		command += " --preview '0,0," + wh.str() + "'";	// x,y,width,height
	}
	else
	{
		ss.str("");
		// Daytime auto-exposure pictures don't need a very long --timeout since the exposures are
		// normally short so the camera can home in on the correct exposure quickly.
		if (autoExposure)
		{
			if (myModeMeanSetting.modeMean && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF)
				ss << 1;	// We do our own auto-exposure so no need to wait at all.
			else if (dayOrNight == "DAY")
				ss << 2 * MS_IN_SEC;
			else	// NIGHT
			{
				// could use longer but it'll take forever for pictures to appear
				ss << 10 * MS_IN_SEC;
			}
		}
		else
		{
			// Manual exposure shots don't need any time to home in since we're specifying the time.
			ss << 1;
		}
		command += " --timeout " + ss.str();
		command += " --nopreview";
	}

//xxxx not sure if this still applies for libcamera
	// https://www.raspberrypi.com/documentation/accessories/camera.html#raspistill
	// Mode		Size		Aspect Ratio	Frame rates		FOV		Binning/Scaling
	// 0		automatic selection
	// 1		2028x1080		169:90		0.1-50fps		Partial	2x2 binned
	// 2		2028x1520		4:3			0.1-50fps		Full	2x2 binned	<<< bin==2
	// 3		4056x3040		4:3			0.005-10fps		Full	None		<<< bin==1
	// 4		1332x990		74:55		50.1-120fps		Partial	2x2 binned	<<< else 

	if (libcamera)
	{
		// xxxx TODO: don't hard code resolutions - use what's defined for camera.
		//	'SRGGB10_CSI2P' : 1332x990 
		//	'SRGGB12_CSI2P' : 2028x1080 2028x1520 4056x3040 
		//								bin 2x2   bin 1x1
		if (bin==1)
			command += " --width 4056 --height 3040";
		else if (bin==2)
			command += " --width 2028 --height 1520";
	}
	else
	{
		if (bin==1)
			command += " --mode 3";
		else if (bin==2)
			command += " --mode 2 --width 2028 --height 1520";
	}

	if (myModeMeanSetting.modeMean && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF)
	{
if (0 && exposure_us != myRaspistillSetting.shutter_us)
{
printf("xxxxxxxxxxx exposure_us = %s != ", length_in_units(exposure_us, true));
printf(" myRaspistillSetting.shutter_us= %s\n", length_in_units(myRaspistillSetting.shutter_us, true));
}
		exposure_us = myRaspistillSetting.shutter_us;
	}

	// Check if automatic determined exposure time is selected
	if (autoExposure)
	{
		if (myModeMeanSetting.modeMean && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF) {
			ss.str("");
			ss << exposure_us;
			if (! libcamera)
				command += " --exposure off";
			command += " --shutter " + ss.str();
		} else {
			// libcamera doesn't use "exposure off/auto".  For auto-exposure set shutter to 0.
			if (libcamera)
				command += " --shutter 0";
			else
				command += " --exposure auto";
		}
	}
	else if (exposure_us)		// manual exposure
	{
		ss.str("");
		ss << exposure_us;
		if (! libcamera)
			command += " --exposure off";
		command += " --shutter " + ss.str();
	}

	// Check if auto gain is selected
	if (autoGain)
	{
		if (myModeMeanSetting.modeMean && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF)
		{
			ss.str("");
			ss << myRaspistillSetting.analoggain;
			command += " --analoggain " + ss.str();
		}
		else if (libcamera)
		{
			command += " --analoggain 0";	// 0 makes it autogain
		}
		else
		{
			command += " --analoggain 1";	// 1 makes it autogain
		}
	}
	else	// Is manual gain
	{
		ss.str("");
		ss << gain;
		command += " --analoggain " + ss.str();
	}

	if (myModeMeanSetting.modeMean && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF) {
		stringstream strExposureTime;
		stringstream strReinforcement;
		strExposureTime <<  myRaspistillSetting.shutter_us;
		strReinforcement << myRaspistillSetting.analoggain;

		command += " --exif IFD0.Artist=li_" + strExposureTime.str() + "_" + strReinforcement.str();
	}

	// libcamera: if the red and blue numbers are given it turns off AWB.
	// Check if R and B component are given
	if (! autoAWB) {
		ss.str("");
		ss << WBR << "," << WBB;
		if (! libcamera)
			command += " --awb off";
		command += " --awbgains " + ss.str();
	}
	else {		// Use automatic white balance
		command += " --awb auto";
	}

	if (rotation != 0) {
		ss.str("");
		ss << rotation;
		command += " --rotation " + ss.str();
	}

	if (flip == 1 || flip == 3)
		command += " --hflip";		// horizontal flip
	if (flip == 2 || flip == 3)
		command += " --vflip";		// vertical flip

	ss.str("");
	ss << saturation;
	command += " --saturation "+ ss.str();

	ss.str("");
	if (libcamera)
		ss << (float) brightness / 100;	// User enters -100 to 100.  Convert to -1.0 to 1.0.
	else
		ss << brightness;
	command += " --brightness " + ss.str();

	ss.str("");
	ss << quality;
	command += " --quality " + ss.str();

	if (libcamera)
	{
		if (debugLevel >= 4)
		{
			command += " > /tmp/capture_RPiHQ_debug.txt 2>&1";
		}
		else
		{
			// Hide verbose libcamera messages that are only needed when debugging.
			command += " 2> /dev/null";	// gets rid of a bunch of libcamera verbose messages
		}
	}

	// Define char variable
	char cmd[command.length() + 1];

	// Convert command to character variable
	strcpy(cmd, command.c_str());

	Log(1, "  > Capture command: %s\n", cmd);

	// Execute the command.
	int ret = system(cmd);
	if (ret == 0)
	{
		*image = cv::imread(fileName, cv::IMREAD_UNCHANGED);
		if (! image->data) {
			printf("WARNING: Error re-reading file '%s'; skipping further processing.\n", basename(fileName));
		}
	}
	return(ret);
}


//-------------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------------

int main(int argc, char *argv[])
{
	// We need to know its value before setting other variables.
	if (argc > 2 && strcmp(argv[1], "-cmd") == 0 && strcmp(argv[2], "libcamera") == 0)
	{
		isLibcamera = true;
	} else {
		isLibcamera = false;
	}

	tty = isatty(fileno(stdout)) ? true : false;
	signal(SIGINT, IntHandle);
	signal(SIGTERM, IntHandle);	// The service sends SIGTERM to end this program.
	signal(SIGHUP, sig);		// xxxxxxxxxx TODO: Re-read settings (we currently just restart).

	int fontname[] = {
		cv::FONT_HERSHEY_SIMPLEX,			cv::FONT_HERSHEY_PLAIN,		cv::FONT_HERSHEY_DUPLEX,
		cv::FONT_HERSHEY_COMPLEX,			cv::FONT_HERSHEY_TRIPLEX,	cv::FONT_HERSHEY_COMPLEX_SMALL,
		cv::FONT_HERSHEY_SCRIPT_SIMPLEX,	cv::FONT_HERSHEY_SCRIPT_COMPLEX };
	char const *fontnames[]		= {		// Character representation of names for clarity:
		"SIMPLEX",							"PLAIN",					"DUPEX",
		"COMPLEX",							"TRIPLEX",					"COMPLEX_SMALL",
		"SCRIPT_SIMPLEX",					"SCRIPT_COMPLEX" };

	char bufTime[128]			= { 0 };
	char const *bayer[]			= { "RG", "BG", "GR", "GB" };
	bool endOfNight				= false;
	int i;
	char const *version			= NULL;		// version of Allsky

	char const *locale			= DEFAULT_LOCALE;
	// All the font settings apply to both day and night.
	long fontnumber				= DEFAULT_FONTNUMBER;
	long iTextX					= DEFAULT_ITEXTX;
	long iTextY					= DEFAULT_ITEXTY;
	long iTextLineHeight		= DEFAULT_ITEXTLINEHEIGHT;
	char const *ImgText			= "";
	char const *ImgExtraText	= "";
	long extraFileAge			= 0;	// 0 disables it
	double fontsize				= DEFAULT_FONTSIZE;
	long linewidth				= DEFAULT_LINEWIDTH;
	bool outlinefont			= DEFAULT_OUTLINEFONT;
	int fontcolor[3]			= { 255, 0, 0 };
	int smallFontcolor[3]		= { 0, 0, 255 };
	int linetype[3]				= { cv::LINE_AA, 8, 4 };
	long linenumber				= DEFAULT_LINENUMBER;
	long width					= DEFAULT_WIDTH;		long originalWidth  = width;
	long height					= DEFAULT_HEIGHT;		long originalHeight = height;
	long dayBin					= DEFAULT_DAYBIN;
	long nightBin				= DEFAULT_NIGHTBIN;
	long imageType				= DEFAULT_IMAGE_TYPE;
	long dayExposure_us			= DEFAULT_DAYEXPOSURE_MS * US_IN_MS;
	long dayMaxAutoexposure_us	= DEFAULT_DAYMAXAUTOEXPOSURE_MS * US_IN_MS;
	long nightExposure_us		= DEFAULT_NIGHTEXPOSURE_MS * US_IN_MS;
	long nightMaxAutoexposure_us= DEFAULT_NIGHTMAXAUTOEXPOSURE_MS * US_IN_MS;
	long currentExposure_us		= NOT_SET;
	long currentMaxAutoexposure_us = NOT_SET;			// _us to match ZWO version
	bool dayAutoExposure		= DEFAULT_DAYAUTOEXPOSURE;
	bool nightAutoExposure		= DEFAULT_NIGHTAUTOEXPOSURE;
	long lastExposure_us 		= 0;					// last exposure taken
	double currentMean			= NOT_SET;
	double dayGain				= DEFAULT_DAYGAIN;		// ISO == gain * 100
	bool dayAutoGain			= DEFAULT_DAYAUTOGAIN;
	double dayMaxGain			= DEFAULT_DAYMAXGAIN;
	double nightGain			= DEFAULT_NIGHTGAIN;
	bool nightAutoGain			= DEFAULT_NIGHTAUTOGAIN;
	double nightMaxGain	 		= DEFAULT_NIGHTMAXGAIN;
	double currentGain			= NOT_SET;
	double currentMaxGain		= NOT_SET;
	double lastGain				= 0.0;					// last gain taken
	long nightDelay_ms			= DEFAULT_NIGHTDELAY_MS;
	long dayDelay_ms			= DEFAULT_DAYDELAY_MS;
	long currentDelay_ms 		= NOT_SET;
	double saturation;
	long dayBrightness;
	long nightBrightness;
	if (isLibcamera)
	{
		defaultSaturation		= 1.0;
		minSaturation			= 0.0;
		maxSaturation			= 2.0;

		defaultBrightness		= 0;
		minBrightness			= -100;
		maxBrightness			= 100;
	}
	else
	{
		defaultSaturation		= 0.0;
		minSaturation			= -100.0;
		maxSaturation			= 100.0;

		defaultBrightness		= 50;
		minBrightness			= 0;
		maxBrightness			= 100;
	}
	saturation					= defaultSaturation;
	dayBrightness				= defaultBrightness;
	nightBrightness				= defaultBrightness;

	long rotation				= 0;
	char const *latitude		= DEFAULT_LATITUDE;
	char const *longitude 		= DEFAULT_LONGITUDE;
	// angle of the sun with the horizon
	// (0=sunset, -6=civil twilight, -12=nautical twilight, -18=astronomical twilight)
	float angle					= DEFAULT_ANGLE;

	bool preview				= false;
	bool showTime				= DEFAULT_SHOWTIME;
	char const *tempType		= "C";							// Celsius
	bool showTemp				= false; // temperature not supported on RPi
	bool showExposure			= DEFAULT_SHOWEXPOSURE;
	bool showGain				= DEFAULT_SHOWGAIN;
	bool showBrightness			= DEFAULT_SHOWBRIGHTNESS;
	bool showMean				= DEFAULT_SHOWMEAN;
	bool showFocus				= DEFAULT_SHOWFOCUS;
	bool takingDarkFrames		= false;
	bool daytimeCapture			= DEFAULT_DAYTIMECAPTURE;
	bool help					= false;
	long quality				= DEFAULT_JPG_QUALITY;
	char const *saveDir			= DEFAULT_SAVEDIR;

	int retCode;
	cv::Mat pRgb;							// the image

	//-------------------------------------------------------------------------------------------------------
	//-------------------------------------------------------------------------------------------------------
	setlinebuf(stdout);		// Line buffer output so entries appear in the log immediately.

	char const *fc = NULL, *sfc = NULL;	// temporary pointers to fontcolor and smallfontcolor
	// These are entered in ms and we convert to us later
	double temp_dayExposure_ms = DEFAULT_DAYEXPOSURE_MS;
	double temp_nightExposure_ms = DEFAULT_NIGHTEXPOSURE_MS;
	double temp_dayMaxAutoexposure_ms = DEFAULT_DAYMAXAUTOEXPOSURE_MS;
	double temp_nightMaxAutoexposure_ms = DEFAULT_NIGHTMAXAUTOEXPOSURE_MS;
	if (argc > 1)
	{
		for (i = 1; i <= argc - 1; i++)
		{
			// Misc. settings
			if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
			{
				help = true;
				quietExit = true;	// we display the help message and quit
			}
			else if (strcmp(argv[i], "-version") == 0)
			{
				version = argv[++i];
			}
			else if (strcmp(argv[i], "-save_dir") == 0)
			{
				saveDir = argv[++i];
			}
			else if (strcmp(argv[i], "-cc_save_dir") == 0)
			{
				CC_saveDir = argv[++i];
				saveCC = true;
				quietExit = true;	// we display info and quit
			}
			else if (strcmp(argv[i], "-cmd") == 0)
			{
				isLibcamera = strcmp(argv[i+1], "libcamera") == 0 ? true : false;
			}
			else if (strcmp(argv[i], "-tty") == 0)
			{
				tty = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-preview") == 0)
			{
				preview = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-daytime") == 0)
			{
				daytimeCapture = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-dayautoexposure") == 0)
			{
				dayAutoExposure = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-daymaxexposure") == 0)
			{
				temp_dayMaxAutoexposure_ms = atof(argv[++i]);	// allow fractions
			}
			else if (strcmp(argv[i], "-dayexposure") == 0)
			{
				temp_dayExposure_ms = atof(argv[++i]);	// allow fractions
			}
			else if (strcmp(argv[i], "-daymean") == 0)
			{
				// If the user specified 0.0, that means don't use modeMean auto exposure/gain.
				double m = atof(argv[++i]);
				if (m > 0.0)
				{
					myModeMeanSetting.dayMean = std::min(1.0,std::max(0.0,m));
					myModeMeanSetting.modeMean = true;
				}
				else
				{
					myModeMeanSetting.dayMean = 0.0;
				}
			}
			else if (strcmp(argv[i], "-daybrightness") == 0)
			{
				dayBrightness = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-daydelay") == 0)
			{
				dayDelay_ms = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-dayautogain") == 0)
			{
				dayAutoGain = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-daymaxgain") == 0)
			{
				dayMaxGain = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-daygain") == 0)
			{
				dayGain = atof(argv[++i]);
			}
 			else if (strcmp(argv[i], "-daybin") == 0)
			{
				dayBin = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-dayawb") == 0)
			{
				dayAutoAWB = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-daywbr") == 0)
			{
				dayWBR = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-daywbb") == 0)
			{
				dayWBB = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-dayskipframes") == 0)
			{
// TODO				daySkipFrames = atol(argv[++i]);
i++;
			}

			// nighttime settings
			else if (strcmp(argv[i], "-nightautoexposure") == 0)
			{
				nightAutoExposure = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightmaxexposure") == 0)
			{
				temp_nightMaxAutoexposure_ms = atof(argv[++i]);	// allow fractions
			}
			else if (strcmp(argv[i], "-nightexposure") == 0)
			{
				temp_nightExposure_ms = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightmean") == 0)
			{
				double m = atof(argv[++i]);
				if (m > 0.0)
				{
					myModeMeanSetting.nightMean = std::min(1.0,std::max(0.0,m));
					myModeMeanSetting.modeMean = true;
				}
				else
				{
					myModeMeanSetting.nightMean = 0.0;
				}
			}
			else if (strcmp(argv[i], "-nightbrightness") == 0)
			{
				nightBrightness = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightdelay") == 0)
			{
				nightDelay_ms = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightautogain") == 0)
			{
				nightAutoGain = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightmaxgain") == 0)
			{
				nightMaxGain = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightgain") == 0)
			{
				nightGain = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightbin") == 0)
			{
				nightBin = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightawb") == 0)
			{
				nightAutoAWB = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightwbr") == 0)
			{
				nightWBR = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightwbb") == 0)
			{
				nightWBB = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-nightskipframes") == 0)
			{
// TODO				nightSkipFrames = atol(argv[++i]);
i++;
			}

			// daytime and nighttime settings
			else if (strcmp(argv[i], "-saturation") == 0)
			{
				saturation = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-width") == 0)
			{
				width = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-height") == 0)
			{
				height = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-type") == 0)
			{
				imageType = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-quality") == 0)
			{
				quality = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-filename") == 0)
			{
				fileName = (argv[++i]);
			}
			else if (strcmp(argv[i], "-rotation") == 0)
			{
				rotation = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-flip") == 0)
			{
				flip = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-notificationimages") == 0)
			{
				notificationImages = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-latitude") == 0)
			{
				latitude = argv[++i];
			}
			else if (strcmp(argv[i], "-longitude") == 0)
			{
				longitude = argv[++i];
			}
			else if (strcmp(argv[i], "-angle") == 0)
			{
				angle = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-darkframe") == 0)
			{
				takingDarkFrames = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-locale") == 0)
			{
				locale = argv[++i];
			}
			else if (strcmp(argv[i], "-debuglevel") == 0)
			{
				debugLevel = atol(argv[++i]);
			}

			// overlay settings
			else if (strcmp(argv[i], "-showTime") == 0)
			{
				showTime = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-timeformat") == 0)
			{
				timeFormat = argv[++i];
			}
			else if (strcmp(argv[i], "-showExposure") == 0)
			{
				showExposure = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-showGain") == 0)
			{
				showGain = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-showBrightness") == 0)
			{
				showBrightness = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-showMean") == 0)
			{
				showMean = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-showFocus") == 0)
			{
				showFocus = getBoolean(argv[++i]);
			}
			else if (strcmp(argv[i], "-text") == 0)
			{
				ImgText = argv[++i];
			}
			else if (strcmp(argv[i], "-extratext") == 0)
			{
				ImgExtraText = argv[++i];
			}
			else if (strcmp(argv[i], "-extratextage") == 0)
			{
				extraFileAge = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-textlineheight") == 0)
			{
				iTextLineHeight = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-textx") == 0)
			{
				iTextX = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-texty") == 0)
			{
				iTextY = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-fontname") == 0)
			{
				fontnumber = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-fontcolor") == 0)
			{
				fc = argv[++i];
			}
			else if (strcmp(argv[i], "-smallfontcolor") == 0)
			{
				sfc = argv[++i];
			}
			else if (strcmp(argv[i], "-fonttype") == 0)
			{
				linenumber = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-fontsize") == 0)
			{
				fontsize = atof(argv[++i]);
			}
			else if (strcmp(argv[i], "-fontline") == 0)
			{
				linewidth = atol(argv[++i]);
			}
			else if (strcmp(argv[i], "-outlinefont") == 0)
			{
				outlinefont = getBoolean(argv[++i]);
			}

			// auto-exposure settings
			else if (strcmp(argv[i], "-mean-threshold") == 0)
			{
				myModeMeanSetting.mean_threshold = std::min(0.1,std::max(0.01,atof(argv[i + 1])));
				i++;
			}
			else if (strcmp(argv[i], "-mean-p0") == 0)
			{
				myModeMeanSetting.mean_p0 = std::min(50.0,std::max(0.0,atof(argv[i + 1])));
				i++;
			}
			else if (strcmp(argv[i], "-mean-p1") == 0)
			{
				myModeMeanSetting.mean_p1 = std::min(50.0,std::max(0.0,atof(argv[i + 1])));
				i++;
			}
			else if (strcmp(argv[i], "-mean-p2") == 0)
			{
				myModeMeanSetting.mean_p2 = std::min(50.0,std::max(0.0,atof(argv[i + 1])));
				i++;
			}

			// Arguments that may be passed to us but we don't use.
			else if (strcmp(argv[i], "-alwaysshowadvanced") == 0)
			{
				i++;
			}
		}
	}

	if (! saveCC)
	{
		printf("\n%s", c(KGRN));
		if (version == NULL) version = "UNKNOWN";
		char v[100]; snprintf(v, sizeof(v), "*** Allsky Camera Software Version %s ***", version);
		for (size_t i=0; i<strlen(v); i++) printf("*");
		printf("\n");
		printf("%s\n", v);
		for (size_t i=0; i<strlen(v); i++) printf("*");
		printf("\n\n");
		printf("Capture images of the sky with a Raspberry Pi and an RPi camera\n");
		printf("%s\n", c(KNRM));
		if (! help) printf("%sAdd -h or --help for available options%s\n\n", c(KYEL), c(KNRM));
		printf("Author: Thomas Jacquin - <jacquin.thomas@gmail.com>\n\n");
		printf("Contributors:\n");
		printf(" -Knut Olav Klo\n");
		printf(" -Daniel Johnsen\n");
		printf(" -Robert Wagner\n");
		printf(" -Michael J. Kidd - <linuxkidd@gmail.com>\n");
		printf(" -Rob Musquetier\n");	
		printf(" -Eric Claeys\n");
		printf(" -Andreas Lindinger\n");
		printf("\n");
	}

	if (setlocale(LC_NUMERIC, locale) == NULL)
		fprintf(stderr, "*** WARNING: Could not set locale to %s ***\n", locale);

	if (help)
	{
		printf("%sUsage:\n", c(KRED));
		printf(" ./capture_RPiHQ -width 640 -height 480 -nightexposure 5000000 -nightbin 1 -filename Lake-Laberge.JPG\n\n");
		printf("%s", c(KNRM));

		printf("%sAvailable Arguments:\n", c(KYEL));
		printf(" -dayautoexposure		- Default = %s: 1 enables daytime auto-exposure\n", yesNo(DEFAULT_DAYAUTOEXPOSURE));
		printf(" -daymaxexposure		- Default = %'d: Maximum daytime auto-exposure in ms (equals to %.1f sec)\n", DEFAULT_DAYMAXAUTOEXPOSURE_MS, (float)DEFAULT_DAYMAXAUTOEXPOSURE_MS/US_IN_MS);
		printf(" -dayexposure			- Default = %'d: Daytime exposure in us (equals to %.4f sec)\n", DEFAULT_DAYEXPOSURE_MS*US_IN_MS, (float)DEFAULT_DAYEXPOSURE_MS/MS_IN_SEC);
		printf(" -daymean				- Default = %.2f: Daytime target exposure brightness\n", DEFAULT_DAYMEAN);
		printf("						  NOTE: Daytime Auto-Gain and Auto-Exposure should be on for best results\n");
		printf(" -daybrightness			- Default = %ld: Daytime brightness level\n", defaultBrightness);
		printf(" -dayDelay				- Default = %'d: Delay between daytime images in milliseconds - 5000 = 5 sec.\n", DEFAULT_DAYDELAY_MS);
		printf(" -dayautogain			- Default = %s: 1 enables daytime auto gain\n", yesNo(DEFAULT_DAYAUTOGAIN));
		printf(" -daymaxgain			- Default = %.2f: Daytime maximum auto gain\n", DEFAULT_DAYMAXGAIN);
		printf(" -daygain				- Default = %.2f: Daytime gain\n", DEFAULT_DAYGAIN);
		printf(" -daybin				- Default = %d: 1 = binning OFF (1x1), 2 = 2x2 binning, 4 = 4x4 binning\n", DEFAULT_DAYBIN);
		printf(" -dayautowhitebalance	- Default = %s: 1 enables auto White Balance\n", yesNo(DEFAULT_DAYAUTOAWB));
		printf(" -daywbr				- Default = %.2f: Manual White Balance Red\n", DEFAULT_DAYWBR);
		printf(" -daywbb				- Default = %.2f: Manual White Balance Blue\n", DEFAULT_DAYWBB);
//		printf(" -dayskipframes			- Default = %d: Number of auto-exposure frames to skip when starting software during daytime.\n", DEFAULT_DAYSKIPFRAMES);

		printf(" -nightautoexposure		- Default = %s: 1 enables nighttime auto-exposure\n", yesNo(DEFAULT_NIGHTAUTOEXPOSURE));
		printf(" -nightmaxexposure		- Default = %'d: Maximum nighttime auto-exposure in ms (equals to %.1f sec)\n", DEFAULT_NIGHTMAXAUTOEXPOSURE_MS, (float)DEFAULT_NIGHTMAXAUTOEXPOSURE_MS/US_IN_MS);
		printf(" -nightexposure			- Default = %'d: Nighttime exposure in us (equals to %.4f sec)\n", DEFAULT_NIGHTEXPOSURE_MS*US_IN_MS, (float)DEFAULT_NIGHTEXPOSURE_MS/MS_IN_SEC);
		printf(" -nightmean				- Default = %.2f: Nighttime target exposure brightness\n", DEFAULT_NIGHTMEAN);
		printf("						  NOTE: Nighttime Auto-Gain and Auto-Exposure should be on for best results\n");
		printf(" -nightbrightness		- Default = %ld: Nighttime brightness level\n", defaultBrightness);
		printf(" -nightDelay			- Default = %'d: Delay between nighttime images in milliseconds - %d = 1 sec.\n", DEFAULT_NIGHTDELAY_MS, MS_IN_SEC);
		printf(" -nightautogain			- Default = %s: 1 enables nighttime auto gain\n", yesNo(DEFAULT_NIGHTAUTOGAIN));
		printf(" -nightmaxgain			- Default = %.2f: Nighttime maximum auto gain\n", DEFAULT_NIGHTMAXGAIN);
		printf(" -nightgain				- Default = %.2f: Nighttime gain\n", DEFAULT_NIGHTGAIN);
		printf(" -nightbin				- Default = %d: same as daybin but for night\n", DEFAULT_NIGHTBIN);
		printf(" -nightautowhitebalance	- Default = %s: 1 enables auto White Balance\n", yesNo(DEFAULT_NIGHTAUTOAWB));
		printf(" -nightwbr				- Default = %.2f: Manual White Balance Red\n", DEFAULT_NIGHTWBR);
		printf(" -nightwbb				- Default = %.2f: Manual White Balance Blue\n", DEFAULT_NIGHTWBB);
//		printf(" -nightskipframes		- Default = %d: Number of auto-exposure frames to skip when starting software during nighttime.\n", DEFAULT_NIGHTSKIPFRAMES);

		printf(" -saturation			- Default = %.1f = Camera Max Width\n", defaultSaturation);
		printf(" -width					- Default = %d = Camera Max Width\n", DEFAULT_WIDTH);
		printf(" -height				- Default = %d = Camera Max Height\n", DEFAULT_HEIGHT);
		printf(" -type = Image Type		- Default = %d: 99 = auto,  0 = RAW8,  1 = RGB24\n", DEFAULT_IMAGE_TYPE);
		printf(" -quality				- Default PNG=3, JPG=%d, Values: PNG=0-9, JPG=0-100\n", DEFAULT_JPG_QUALITY);
		printf(" -filename				- Default = %s\n", DEFAULT_FILENAME);
		if (isLibcamera)
			printf(" -rotation							- Default = 0 degrees - Options 0 or 180\n");
		else
			printf(" -rotation							- Default = 0 degrees - Options 0, 90, 180 or 270\n");
		printf(" -flip					- Default = 0: 0 = No flip, 1 = Horizontal, 2 = Vertical, 3 = Both\n");
		printf(" -notificationimages	- 1 enables notification images, for example, 'Camera is off during day'.\n");
		printf(" -latitude				- No default - you must set it.  Latitude of the camera.\n");
		printf(" -longitude				- No default - you must set it.  Longitude of the camera.\n");
		printf(" -angle					- Default = %.2f: Angle of the sun below the horizon.\n", DEFAULT_ANGLE);
		printf("		-6=civil twilight   -12=nautical twilight   -18=astronomical twilight\n");
		printf(" -darkframe				- 1 disables the overlay and takes dark frames instead\n");
		printf(" -locale				- Default = %s: Your locale - to determine thousands separator and decimal point.\n", DEFAULT_LOCALE);
		printf("						  Type 'locale' at a command prompt to determine yours.\n");
		printf(" -debuglevel			- Default = 0. Set to 1,2, 3, or 4 for more debugging information.\n");

		printf(" -showTime				- Set to 1 to display the time on the image.\n");
		printf(" -timeformat			- Format the optional time is displayed in; default is '%s'\n", DEFAULT_TIMEFORMAT);
		printf(" -showExposure			- 1 displays the exposure length\n");
		printf(" -showGain				- 1 display the gain\n");
		printf(" -showBrightness		- 1 displays the brightness\n");
		printf(" -showMean				- 1 displays the mean brightness\n");
		printf(" -focus					- Set to 1 to display a focus metric on the image.\n");
		printf(" -text					- Default = \"\": Text Overlay\n");
		printf(" -extratext				- Default = \"\": Full Path to extra text to display\n");
		printf(" -extratextage			- Default = 0: If the extra file is not updated after this many seconds its contents will not be displayed. 0 disables it.\n");
		printf(" -textlineheight		- Default = %d: Text Line Height in pixels\n", DEFAULT_ITEXTLINEHEIGHT);
		printf(" -textx					- Default = %d: Text Placement Horizontal from LEFT in pixels\n", DEFAULT_ITEXTX);
		printf(" -texty					- Default = %d: Text Placement Vertical from TOP in pixels\n", DEFAULT_ITEXTY);
		printf(" -fontname				- Default = %d: Font Types (0-7), Ex. 0 = simplex, 4 = triplex, 7 = script\n", DEFAULT_FONTNUMBER);
		printf(" -fontcolor				- Default = 255 0 0: Text font color (BGR)\n");
		printf(" -smallfontcolor		- Default = 0 0 255: Small text font color (BGR)\n");
		printf(" -fonttype				- Default = %d: Font Line Type: 0=AA, 1=8, 2=4\n", DEFAULT_LINENUMBER);
		printf(" -fontsize				- Default = %d: Text Font Size\n", DEFAULT_FONTSIZE);
		printf(" -fontline				- Default = %d: Text Font Line Thickness\n", DEFAULT_LINEWIDTH);
		printf(" -outlinefont			- Default = %s: 1 enables outline font\n", yesNo(DEFAULT_OUTLINEFONT));

		printf("\n");
		printf(" -daytime				- Default = %s: 1 enables capture daytime images\n", yesNo(DEFAULT_DAYTIMECAPTURE));
		printf(" -save_dir directory	- Default = %s: where to save 'filename'\n", DEFAULT_SAVEDIR);
		printf(" -preview				- 1 previews the captured images. Only works with a Desktop Environment\n");
		printf(" -cc_save_dir directory	- Outputs the camera's capabilities to the specified directory and exists.\n");
		printf(" -version				- Version of Allsky in use.\n");
		printf(" -cmd					- Command being used to take pictures (Buster: raspistill, Bullseye: libcamera-still\n");
		printf(" -mean-threshold		- Default = %.2f: Set mean-value and activates exposure control\n", DEFAULT_MEAN_THRESHOLD);
		printf(" -mean-p0				- Default = %.1f: Be careful changing these values, ExposureChange (Steps) = p0 + p1 * diff + (p2*diff)^2\n", DEFAULT_MEAN_P0);
		printf(" -mean-p1				- Default = %.1f\n", DEFAULT_MEAN_P1);
		printf(" -mean-p2				- Default = %.1f\n", DEFAULT_MEAN_P2);

		printf("%s", c(KNRM));
		exit(EXIT_OK);
	}

	// Do argument error checking if we're not going to exit soon.
	// Some checks are done lower in the code, after we processed some values.
const int minExposure_us = 1;	// TODO: determine based on camera
const double minGain = 1.0;		// TODO: determine based on camera
	if (! saveCC)
	{
		// xxxx TODO: NO_MAX_VALUE will be replaced by acutal values
		validateFloat(&temp_dayMaxAutoexposure_ms, minExposure_us/US_IN_MS, NO_MAX_VALUE, "Daytime Max Auto-Exposure", true);	// camera-specific
			dayMaxAutoexposure_us = temp_dayMaxAutoexposure_ms * US_IN_MS;
		validateFloat(&temp_dayExposure_ms, minExposure_us/US_IN_MS, dayAutoExposure ? (dayMaxAutoexposure_us/US_IN_MS) : NO_MAX_VALUE, "Daytime Manual Exposure", true);
			dayExposure_us = temp_dayExposure_ms * US_IN_MS;
		validateLong(&dayBrightness, minBrightness, maxBrightness, "Daytime Brightness", true);
		validateLong(&dayDelay_ms, 10, NO_MAX_VALUE, "Daytime Delay", false);
		validateFloat(&dayMaxGain, 1, NO_MAX_VALUE, "Daytime Max Auto-Gain", true);					// camera-specific
		validateFloat(&dayGain, 1, dayAutoGain ? dayMaxGain : NO_MAX_VALUE, "Daytime Gain", true);
		validateLong(&dayBin, 1, 3, "Daytime Binning", false);								// camera-specific
		validateFloat(&dayWBR, 0, NO_MAX_VALUE, "Daytime Red Balance", true);							// camera-specific
		validateFloat(&dayWBB, 0, NO_MAX_VALUE, "Daytime Blue Balance", true);						// camera-specific
//		validateLong(&daySkipFrames, 0, 50, "Daytime Skip Frames", true);

		validateFloat(&temp_nightMaxAutoexposure_ms, minExposure_us*US_IN_MS, NO_MAX_VALUE, "Nighttime Max Auto-Exposure", true);	// camera-specific
			nightMaxAutoexposure_us = temp_nightMaxAutoexposure_ms * US_IN_MS;
		validateFloat(&temp_nightExposure_ms, minExposure_us*US_IN_MS, nightAutoExposure ? (nightMaxAutoexposure_us/US_IN_MS) : NO_MAX_VALUE, "Nighttime Manual Exposure", true);
			nightExposure_us = temp_nightExposure_ms * US_IN_MS;
		validateLong(&nightBrightness, minBrightness, maxBrightness, "Nighttime Brightness", true);
		validateLong(&nightDelay_ms, 10, NO_MAX_VALUE, "Nighttime Delay", false);
		validateFloat(&nightMaxGain, 0, NO_MAX_VALUE, "Nighttime Max Auto-Gain", true);				// camera-specific
		validateFloat(&nightGain, 0, nightAutoGain ? nightMaxGain : NO_MAX_VALUE, "Nighttime Gain", true);
		validateLong(&nightBin, 1, 3, "Nighttime Binning", false);							// camera-specific
		validateFloat(&nightWBR, 0, NO_MAX_VALUE, "Nighttime Red Balance", true);						// camera-specific
		validateFloat(&nightWBB, 0, NO_MAX_VALUE, "Nighttime Blue Balance", true);					// camera-specific
//		validateLong(&nightSkipFrames, 0, 50, "Nighttime Skip Frames", true);

		validateFloat(&saturation, minSaturation, maxSaturation, "Saturation", true);
		validateLong(&rotation, 0, 180, "Rotation", true);
		if (imageType != AUTO_IMAGE_TYPE)
			validateLong(&imageType, 0, ASI_IMG_END, "Image Type", false);
		validateLong(&flip, 0, 3, "Flip", false);
		validateLong(&debugLevel, 0, 5, "Debug Level", true);

		validateLong(&extraFileAge, 0, NO_MAX_VALUE, "Max Age Of Extra", true);
		validateLong(&fontnumber, 0, sizeof(fontname)-1, "Font Name", true);
		validateLong(&linenumber, 0, sizeof(linetype)-1, "Font Smoothness", true);

		if (fc != NULL && sscanf(fc, "%d %d %d", &fontcolor[0], &fontcolor[1], &fontcolor[2]) != 3)
			Log(-1, "%s*** WARNING: Not enough font color parameters: '%s'%s\n", c(KRED), fc, c(KNRM));
		if (sfc != NULL && sscanf(sfc, "%d %d %d", &smallFontcolor[0], &smallFontcolor[1], &smallFontcolor[2]) != 3)
			Log(-1, "%s*** WARNING: Not enough small font color parameters: '%s'%s\n", c(KRED), sfc, c(KNRM));

		// libcamera only supports 0 and 180 degree rotation
		if (rotation != 0)
		{
			if (isLibcamera)
			{
				if (rotation != 180)
				{
					Log(0, "%s*** ERROR: Only 0 and 180 degrees are supported for rotation; you entered %ld.%s\n", c(KRED), rotation, c(KNRM));
					closeUp(EXIT_ERROR_STOP);
				}
			}
			else if (rotation != 90 && rotation != 180 && rotation != 270)
			{
				Log(0, "%s*** ERROR: Only 0, 90, 180, and 270 degrees are supported for rotation; you entered %ld.%s\n", c(KRED), rotation, c(KNRM));
				closeUp(EXIT_ERROR_STOP);
			}
		}
	}

	char const *imagetype = "";
	char const *ext = checkForValidExtension(fileName, imageType);
	if (ext == NULL)
	{
		// checkForValidExtension() displayed the error message.
		closeUp(EXIT_ERROR_STOP);
	}
	if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
	{
		imagetype = "jpg";
		compressionParameters.push_back(cv::IMWRITE_JPEG_QUALITY);
		// want dark frames to be at highest quality
		if (takingDarkFrames)
		{
			quality = 100;
		}
		else if (quality == NOT_SET)
		{
			quality = DEFAULT_JPG_QUALITY;
		}
		else
		{
			validateLong(&quality, 0, 100, "JPG Quality", true);
		}
	}
	else if (strcasecmp(ext, "png") == 0)
	{
		imagetype = "png";
		compressionParameters.push_back(cv::IMWRITE_PNG_COMPRESSION);
		// png is lossless so "quality" is really just the amount of compression.
		if (takingDarkFrames)
		{
			quality = 9;
		}
		else if (quality == NOT_SET)
		{
			quality = DEFAULT_PNG_COMPRESSION;
		}
		else
		{
			validateLong(&quality, 0, 9, "PNG Quality/Compression", true);
		}
	}
	compressionParameters.push_back(quality);

	// Get just the name of the file, without any directories or the extension.
	char fileNameOnly[50] = { 0 };
	if (takingDarkFrames)
	{
		// To avoid overwriting the optional notification image with the dark image,
		// during dark frames we use a different file name.
		static char darkFilename[20];
		sprintf(darkFilename, "dark.%s", imagetype);
		fileName = darkFilename;
		strncat(finalFileName, fileName, sizeof(finalFileName)-1);
	}
	else
	{
		char const *slash = strrchr(fileName, '/');
		if (slash == NULL)
			strncat(fileNameOnly, fileName, sizeof(fileNameOnly)-1);
		else
			strncat(fileNameOnly, slash + 1, sizeof(fileNameOnly)-1);
		char *dot = strrchr(fileNameOnly, '.');	// we know there's an extension
		*dot = '\0';
	}

	processConnectedCameras();	// exits on error

	ASI_CAMERA_INFO ASICameraInfo;
	ASIGetCameraProperty(&ASICameraInfo, CamNum);

	int iMaxWidth, iMaxHeight;
	double pixelSize;
	iMaxWidth  = ASICameraInfo.MaxWidth;
	iMaxHeight = ASICameraInfo.MaxHeight;
	pixelSize  = ASICameraInfo.PixelSize;
	if (width == 0 || height == 0)
	{
		width  = iMaxWidth;
		height = iMaxHeight;
	}
	else
	{
		validateLong(&width, 0, iMaxWidth, "Width", false);
		validateLong(&height, 0, iMaxHeight, "Height", false);
	}
	originalWidth = width;
	originalHeight = height;
	// Limit these to a reasonable value based on the size of the sensor.
	validateLong(&iTextLineHeight, 0, (long)(iMaxHeight / 2), "Line Height", true);
	validateLong(&iTextX, 0, (long)iMaxWidth - 10, "Text X", true);
	validateLong(&iTextY, 0, (long)iMaxHeight - 10, "Text Y", true);
	validateFloat(&fontsize, 0.1, iMaxHeight / 2, "Font Size", true);
	validateLong(&linewidth, 0, (long)(iMaxWidth / 2), "Font Weight", true);

	ASIGetNumOfControls(CamNum, &iNumOfCtrl);

	if (saveCC)
	{
		saveCameraInfo(ASICameraInfo, CC_saveDir, iMaxWidth, iMaxHeight, pixelSize, bayer[ASICameraInfo.BayerPattern]);
		closeUp(EXIT_OK);
	}

	outputCameraInfo(ASICameraInfo, iMaxWidth, iMaxHeight, pixelSize, bayer[ASICameraInfo.BayerPattern]);
	// checkExposureValues() must come after outputCameraInfo().
	(void) checkExposureValues(&dayExposure_us, dayAutoExposure, &nightExposure_us, nightAutoExposure);

	// Handle "auto" imageType.
	if (imageType == AUTO_IMAGE_TYPE)
	{
		// user will have to manually set for 8- or 16-bit mono mode
		imageType = IMG_RGB24;
	}

	char const *sType;		// displayed in output
	if (imageType == IMG_RAW16)
	{
		sType = "RAW16";
		currentBpp = 2;
		currentBitDepth = 16;
	}
	else if (imageType == IMG_RGB24)
	{
		sType = "RGB24";
		currentBpp = 3;
		currentBitDepth = 8;
	}
	else if (imageType == IMG_RAW8)
	{
		sType = "RAW8";
		currentBpp = 1;
		currentBitDepth = 8;
	}
	else
	{
		Log(0, "*** ERROR: Unknown Image Type: %d\n", imageType);
		exit(EXIT_ERROR_STOP);
	}

	//-------------------------------------------------------------------------------------------------------
	//-------------------------------------------------------------------------------------------------------

	printf("%s", c(KGRN));
	printf("\nCapture Settings:\n");
	printf(" Command: %s\n", getCameraCommand(isLibcamera));
	printf(" Image Type: %s\n", sType);
	printf(" Resolution (before any binning): %ldx%ld\n", width, height);
	printf(" Quality: %ld\n", quality);
	printf(" Daytime capture: %s\n", yesNo(daytimeCapture));

	printf(" Exposure (day):   %s, Auto: %s", length_in_units(dayExposure_us, true), yesNo(dayAutoExposure));
		if (dayAutoExposure)
			printf(", Max Auto-Exposure: %s\n", length_in_units(dayMaxAutoexposure_us, true));
		printf("\n");
	printf(" Exposure (night): %s, Auto: %s", length_in_units(nightExposure_us, true), yesNo(nightAutoExposure));
		if (nightAutoExposure)
			printf(", Max Auto-Exposure: %s\n", length_in_units(nightMaxAutoexposure_us, true));
		printf("\n");
	printf(" Gain (day):   %1.2f, Auto: %s", dayGain, yesNo(dayAutoGain));
		if (dayAutoGain)
			printf(", Max Auto-Gain: %1.2f\n", dayMaxGain);
		printf("\n");
	printf(" Gain (night): %1.2f, Auto: %s", nightGain, yesNo(nightAutoGain));
		if (nightAutoGain)
			printf(", Max Auto-Gain: %1.2f\n", nightMaxGain);
		printf("\n");
	printf(" Brightness (day):   %ld\n", dayBrightness);
	printf(" Brightness (night): %ld\n", nightBrightness);
	printf(" Binning (day):   %ld\n", dayBin);
	printf(" Binning (night): %ld\n", nightBin);
	printf(" White Balance (day)   Red: %.2f, Blue: %.2f, Auto: %s\n", dayWBR, dayWBB, yesNo(dayAutoAWB));
	printf(" White Balance (night) Red: %.2f, Blue: %.2f, Auto: %s\n", nightWBR, nightWBB, yesNo(nightAutoAWB));
	printf(" Delay (day):   %s\n", length_in_units(dayDelay_ms*US_IN_MS, true));
	printf(" Delay (night): %s\n", length_in_units(nightDelay_ms*US_IN_MS, true));

	printf(" Saturation: %.1f\n", saturation);
	printf(" Rotation: %ld\n", rotation);
	printf(" Flip Image: %s (%ld)\n", getFlip(flip), flip);
	printf(" Filename: %s\n", fileName);
	printf(" Filename Save Directory: %s\n", saveDir);
	printf(" Latitude: %s, Longitude: %s\n", latitude, longitude);
	printf(" Sun Elevation: %.4f\n", angle);
	printf(" Locale: %s\n", locale);
	printf(" Notification Images: %s\n", yesNo(notificationImages));
	printf(" Preview: %s\n", yesNo(preview));
	printf(" Taking Dark Frames: %s\n", yesNo(takingDarkFrames));
	printf(" Debug Level: %ld\n", debugLevel);
	printf(" On TTY: %s\n", yesNo(tty));

	printf(" Text Overlay: %s\n", ImgText[0] == '\0' ? "[none]" : ImgText);
	printf(" Text Extra File: %s, Age: %ld seconds\n", ImgExtraText[0] == '\0' ? "[none]" : ImgExtraText, extraFileAge);
	printf(" Text Line Height %ldpx\n", iTextLineHeight);
	printf(" Text Position: %ldpx from left, %ldpx from top\n", iTextX, iTextY);
	printf(" Font Name: %s (%d)\n", fontnames[fontnumber], fontname[fontnumber]);
	printf(" Font Color: %d, %d, %d\n", fontcolor[0], fontcolor[1], fontcolor[2]);
	printf(" Small Font Color: %d, %d, %d\n", smallFontcolor[0], smallFontcolor[1], smallFontcolor[2]);
	printf(" Font Line Type: %d\n", linetype[linenumber]);
	printf(" Font Size: %1.1f\n", fontsize);
	printf(" Font Line Width: %ld\n", linewidth);
	printf(" Outline Font : %s\n", yesNo(outlinefont));

	printf(" Show Time: %s (format: %s)\n", yesNo(showTime), timeFormat);
	printf(" Show Exposure: %s\n", yesNo(showExposure));
	printf(" Show Gain: %s\n", yesNo(showGain));
	printf(" Show Brightness: %s\n", yesNo(showBrightness));
	printf(" Show Mean Brightness: %s\n", yesNo(showMean));
	printf(" Show Focus Metric: %s\n", yesNo(showFocus));
	printf(" Mode Mean: %s\n", yesNo(myModeMeanSetting.modeMean));
	if (myModeMeanSetting.modeMean) {
		printf("    Mean Value (night): %1.3f\n", myModeMeanSetting.nightMean);
		printf("    Mean Value (day):   %1.3f\n", myModeMeanSetting.dayMean);
		printf("    Threshold: %1.3f\n", myModeMeanSetting.mean_threshold);
		printf("    p0: %1.3f\n", myModeMeanSetting.mean_p0);
		printf("    p1: %1.3f\n", myModeMeanSetting.mean_p1);
		printf("    p2: %1.3f\n", myModeMeanSetting.mean_p2);
	}
	printf(" Allsky version: %s\n", version);
	printf("%s", c(KNRM));

	// Initialization
	int originalITextX = iTextX;
	int originalITextY = iTextY;
	int originalFontsize = fontsize;
	int originalLinewidth = linewidth;
	// Have we displayed "not taking picture during day" message, if applicable?
	bool displayedNoDaytimeMsg = false;

	while (bMain)
	{
		// Find out if it is currently DAY or NIGHT
		dayOrNight = calculateDayOrNight(latitude, longitude, angle);
		std::string lastDayOrNight = dayOrNight;

		if (takingDarkFrames) {
			// We're doing dark frames so turn off autoexposure and autogain, and use
			// nightime gain, delay, exposure, and brightness to mimic a nightime shot.
			currentAutoExposure = false;
			nightAutoExposure = false;
			currentAutoGain = false;
			currentGain = nightGain;
			currentMaxGain = nightMaxGain;		// not needed since we're not using auto gain, but set to be consistent
			currentDelay_ms = nightDelay_ms;
			currentMaxAutoexposure_us = currentExposure_us = nightMaxAutoexposure_us;
			currentBin = nightBin;
			currentBrightness = nightBrightness;
			currentAutoAWB = false;
			currentWBR = nightWBR;
			currentWBB = nightWBB;
			currentMean = NOT_SET;

 			Log(0, "Taking dark frames...\n");

			if (notificationImages) {
				system("scripts/copy_notification_image.sh --expires 0 DarkFrames &");
			}
		}

		else if (dayOrNight == "DAY")
		{
			if (endOfNight == true)		// Execute end of night script
			{
				Log(0, "Processing end of night data\n");
				system("scripts/endOfNight.sh &");
				endOfNight = false;
				displayedNoDaytimeMsg = false;
			}

			if (! daytimeCapture)
			{
				// Only display messages once a day.
				if (! displayedNoDaytimeMsg) {
					if (notificationImages) {
						system("scripts/copy_notification_image.sh --expires 0 CameraOffDuringDay &");
					}
					Log(0, "It's daytime... we're not saving images.\n%s",
						tty ? "*** Press Ctrl+C to stop ***\n" : "");
					displayedNoDaytimeMsg = true;

					// sleep until around nighttime, then wake up and sleep more if needed.
					int secsTillNight = calculateTimeToNightTime(latitude, longitude, angle);
					timeval t;
					t = getTimeval();
					t.tv_sec += secsTillNight;
					Log(1, "Sleeping until %s (%'d seconds)\n", formatTime(t, timeFormat), secsTillNight);
					sleep(secsTillNight);
				}
				else
				{
					// Shouldn't need to sleep more than a few times before nighttime.
					int s = 5;
					Log(1, "Not quite nighttime; sleeping %'d more seconds\n", s);
					sleep(s);
				}

				// No need to do any of the code below so go back to the main loop.
				continue;
			}

			else
			{
				Log(0, "==========\n=== Starting daytime capture ===\n==========\n");

				currentExposure_us = dayExposure_us;
				currentMaxAutoexposure_us = dayMaxAutoexposure_us;
				currentAutoExposure = dayAutoExposure;
				currentBrightness = dayBrightness;
				currentAutoAWB = dayAutoAWB;
				currentWBR = dayWBR;
				currentWBB = dayWBB;
				currentDelay_ms = dayDelay_ms;
				currentBin = dayBin;
				currentGain = dayGain;
				currentMaxGain = dayMaxGain;
				currentAutoGain = dayAutoGain;
				currentMean = myModeMeanSetting.dayMean;
			}
		}

		else	// NIGHT
		{
			Log(0, "==========\n=== Starting nighttime capture ===\n==========\n");

			// Setup the night time capture parameters
			currentExposure_us = nightExposure_us;
			currentAutoExposure = nightAutoExposure;
			currentBrightness = nightBrightness;
			if (ASICameraInfo.IsColorCam)
			{
				currentAutoAWB = nightAutoAWB;
				currentWBR = nightWBR;
				currentWBB = nightWBB;
			}
			currentDelay_ms = nightDelay_ms;
			currentBin = nightBin;
			currentMaxAutoexposure_us = nightMaxAutoexposure_us;
			currentGain = nightGain;
			currentMaxGain = nightMaxGain;
			currentAutoGain = nightAutoGain;
			currentMean = myModeMeanSetting.nightMean;
		}

		if (currentMean > 0.0)
		{
			myModeMeanSetting.modeMean = true;
			myModeMeanSetting.meanValue = currentMean;
			if (! aegInit(currentAutoExposure, minExposure_us, currentMaxAutoexposure_us, currentExposure_us,
				currentAutoGain, minGain, currentMaxGain, currentGain,
				myRaspistillSetting, myModeMeanSetting))
			{
				closeUp(EXIT_ERROR_STOP);
			}
		}
		else
		{
			myModeMeanSetting.modeMean = false;
		}

		if (currentAutoExposure && currentExposure_us > currentMaxAutoexposure_us)
		{
			currentExposure_us = currentMaxAutoexposure_us;
		}

		// Want initial exposures to have the exposure time and gain the user specified.
		if (numExposures == 0)
		{
			myRaspistillSetting.shutter_us = currentExposure_us;
			myRaspistillSetting.analoggain = currentGain;
		}

		// Adjusting variables for chosen binning
		height		= originalHeight / currentBin;
		width		= originalWidth / currentBin;
		iTextX		= originalITextX / currentBin;
		iTextY		= originalITextY / currentBin;
		fontsize	= originalFontsize / currentBin;
		linewidth	= originalLinewidth / currentBin;

// TODO: if not the first time, should we free the old pRgb?
		if (imageType == IMG_RAW16)
		{
			pRgb.create(cv::Size(width, height), CV_16UC1);
		}
		else if (imageType == IMG_RGB24)
		{
			pRgb.create(cv::Size(width, height), CV_8UC3);
		}
		else // RAW8 and Y8
		{
			pRgb.create(cv::Size(width, height), CV_8UC1);
		}

		// Wait for switch day time -> night time or night time -> day time
		while (bMain && lastDayOrNight == dayOrNight)
		{
			// date/time is added to many log entries to make it easier to associate them
			// with an image (which has the date/time in the filename).
			exposureStartDateTime = getTimeval();
			char exposureStart[128];
			snprintf(exposureStart, sizeof(exposureStart), "%s", formatTime(exposureStartDateTime, "%F %T"));
			Log(0, "STARTING EXPOSURE at: %s   @ %s\n", exposureStart, length_in_units(myRaspistillSetting.shutter_us, true));

			// Get start time for overlay. Make sure it has the same time as exposureStart.
			if (showTime)
			{
				sprintf(bufTime, "%s", formatTime(exposureStartDateTime, timeFormat));
			}

			if (! takingDarkFrames)
			{
				// Create the name of the file that goes in the images/<date> directory.
				snprintf(finalFileName, sizeof(finalFileName), "%s-%s.%s",
					fileNameOnly, formatTime(exposureStartDateTime, "%Y%m%d%H%M%S"), imagetype);
			}
			snprintf(fullFilename, sizeof(fullFilename), "%s/%s", saveDir, finalFileName);

			// Capture and save image
			retCode = RPiHQcapture(currentAutoExposure, currentExposure_us, currentBin, currentAutoGain, currentGain, currentAutoAWB, currentWBR, currentWBB, rotation, flip, saturation, currentBrightness, quality, fullFilename, takingDarkFrames, preview, width, height, isLibcamera, &pRgb, imagetype);

			if (retCode == 0)
			{
				numExposures++;

				int focusMetric;
				focusMetric = showFocus ? (int)round(get_focus_metric(pRgb)) : -1;

				// If takingDarkFrames is off, add overlay text to the image
				if (! takingDarkFrames)
				{
					lastExposure_us = myRaspistillSetting.shutter_us;
					if (myModeMeanSetting.modeMean && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF)
						lastGain =  myRaspistillSetting.analoggain;
					else
						lastGain = currentGain;	// ZWO gain=0.1 dB , RPiHQ gain=factor

					mean = -1;
					if (myModeMeanSetting.modeMean && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF)
					{
if (lastExposure_us != myRaspistillSetting.shutter_us)
  Log(0, " xxxx lastExposure_us (%ld) != shutter_us (%ld)\n", lastExposure_us, myRaspistillSetting.shutter_us);
//xxx						mean = aegCalcMean(pRgb, currentExposure_us, currentGain,
						mean = aegCalcMean(pRgb, lastExposure_us, lastGain,
									myRaspistillSetting, myModeMeanSetting);
						Log(2, "  > Got exposure: %s, gain: %1.3f,", length_in_units(lastExposure_us, false), lastGain);
						Log(2, " shutter: %s, quickstart: %d, mean=%1.3f\n", length_in_units(myRaspistillSetting.shutter_us, false), myModeMeanSetting.quickstart, mean);
						if (mean == -1)
						{
							numErrors++;
							Log(-1, "ERROR: aegCalcMean() returned mean of -1.\n");
							Log(1, "  > Sleeping from failed exposure: %.1f seconds\n", (float)currentDelay_ms / MS_IN_SEC);
							usleep(currentDelay_ms * US_IN_MS);
							continue;
						}
					}
					else {
						myRaspistillSetting.shutter_us = currentExposure_us;
						myRaspistillSetting.analoggain = currentGain;
					}

					if (doOverlay(pRgb,
						showTime, bufTime,
						showExposure, lastExposure_us, currentAutoExposure,
						showTemp, actualTemp, tempType,
 						showGain, lastGain, currentAutoGain, 0,
						(showMean && mean != -1), mean,
						showBrightness, currentBrightness,
						showFocus, focusMetric,
						ImgText, ImgExtraText, extraFileAge,
						iTextX, iTextY, currentBin, width, iTextLineHeight,
						fontsize, linewidth, linetype[linenumber], fontname[fontnumber],
						fontcolor, smallFontcolor, outlinefont, imageType) > 0)
					{
						// if we added anything to overlay, write the file out
						bool result = cv::imwrite(fullFilename, pRgb, compressionParameters);
						if (! result) fprintf(stderr, "*** ERROR: Unable to write to '%s'\n", fullFilename);
					}
				}

				char cmd[1100];
				Log(1, "  > Saving %s image '%s'\n", takingDarkFrames ? "dark" : dayOrNight.c_str(), finalFileName);
				snprintf(cmd, sizeof(cmd), "scripts/saveImage.sh %s '%s'", dayOrNight.c_str(), fullFilename);

				// TODO: in the future the calculation of mean should independent from modeMean. -1 means don't display.
				float m = (myModeMeanSetting.modeMean && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF) ? mean : -1.0;
				add_variables_to_command(cmd, exposureStartDateTime,
					lastExposure_us, currentBrightness, m,
					currentAutoExposure, currentAutoGain, currentAutoAWB, currentWBR, currentWBB,
					actualTemp, lastGain, currentBin, getFlip(flip), currentBitDepth, focusMetric);
				strcat(cmd, " &");

				system(cmd);

				long s;
				if (myModeMeanSetting.modeMean && myModeMeanSetting.quickstart && myModeMeanSetting.meanAuto != MEAN_AUTO_OFF)
				{
					s = 1 * US_IN_SEC;
				}
				else if ((dayOrNight == "NIGHT"))
				{
					s = (nightExposure_us - myRaspistillSetting.shutter_us) + (nightDelay_ms * US_IN_MS);
				}
				else
				{
					s = currentDelay_ms * US_IN_MS;
				}
				Log(0, "Sleeping %.1f seconds...\n", (float)s / US_IN_SEC);
				usleep(s);
			}
			else
			{
				numErrors++;
				int r = retCode >> 8;
				printf(" >>> Unable to take picture, return code=%d, r=%d\n", retCode, r);
				if (WIFSIGNALED(r)) r = WTERMSIG(r);
				{
					// Got a signal.  See if it's one we care about.
					std::string z = "";
					if (r == SIGINT) z = "SIGINT";
					else if (r == SIGTERM) z = "SIGTERM";
					else if (r == SIGHUP) z = "SIGHUP";
					if (z != "")
					{
						printf("xxxx Got %s in %s\n", z.c_str(), getCameraCommand(isLibcamera));
					}
					else
					{
						printf("xxxx Got signal %d in capture_RPiHQ.cpp\n", r);
					}
				}
				// Don't wait the full amount on error.
				long timeToSleep = (float)currentDelay_ms * .25;
				Log(1, "  > Sleeping from failed exposure: %.1f seconds\n", (float)timeToSleep / MS_IN_SEC);
				usleep(timeToSleep * US_IN_MS);
			}

			// Check for day or night based on location and angle
			dayOrNight = calculateDayOrNight(latitude, longitude, angle);
		}

		if (lastDayOrNight == "NIGHT")
		{
			// Flag end of night processing is needed
			endOfNight = true;
		}
	}

	closeUp(EXIT_OK);
}
