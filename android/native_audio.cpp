#include <dlfcn.h>

#include "base/logging.h"
#include "android/native_audio.h"
#include "android/native-audio-so.h"

static void *so;
static OpenSLWrap_Init_T init_func;
static OpenSLWrap_Shutdown_T shutdown_func;

bool AndroidAudio_Init(AndroidAudioCallback cb) {
	ILOG("Loading native audio library...");
	so = dlopen("libnative_audio.so", RTLD_NOW);
	if (!so) {
		ELOG("Failed to find native audio library");
		return false;
	}
	init_func = (OpenSLWrap_Init_T)dlsym(so, "OpenSLWrap_Init");
	shutdown_func = (OpenSLWrap_Shutdown_T)dlsym(so, "OpenSLWrap_Shutdown");

	ILOG("Calling OpenSLWrap_Init_T...");
	bool init_retval = init_func(cb);
	ILOG("Returned from OpenSLWrap_Init_T");
	return init_retval;
}


void AndroidAudio_Shutdown() {
	ILOG("Calling OpenSLWrap_Shutdown_T...");
	shutdown_func();
	ILOG("Returned from OpenSLWrap_Shutdown_T");
	dlclose(so);
	so = 0;
}
