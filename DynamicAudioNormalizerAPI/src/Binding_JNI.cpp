//////////////////////////////////////////////////////////////////////////////////
// Dynamic Audio Normalizer - Audio Processing Library
// Copyright (c) 2014 LoRd_MuldeR <mulder2@gmx.de>. Some rights reserved.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
//
// http://www.gnu.org/licenses/lgpl-2.1.txt
//////////////////////////////////////////////////////////////////////////////////

#include "DynamicAudioNormalizer.h"

//StdLib
#include <algorithm>
#include <stdexcept>
#include <map>
#include <queue>
#include <climits>

//Generate the JNI header file name
#define JAVA_HDRNAME_GLUE1(X,Y) <##X##_r##Y##.h>
#define JAVA_HDRNAME_GLUE2(X,Y) JAVA_HDRNAME_GLUE1(X,Y)
#define JAVA_HDRNAME(X) JAVA_HDRNAME_GLUE2(X, MDYNAMICAUDIONORMALIZER_CORE)

//Generate JNI export names and corresponding helper functions
#define JAVA_FUNCTION_GLUE1(X,Y,Z) X##Y##_##Z
#define JAVA_FUNCTION_GLUE2(X,Y,Z) JAVA_FUNCTION_GLUE1(X,Y,Z)
#define JAVA_FUNCTION(X) JAVA_FUNCTION_GLUE2(Java_com_muldersoft_dynaudnorm_JDynamicAudioNormalizer_00024NativeAPI_1r, MDYNAMICAUDIONORMALIZER_CORE, X)
#define JAVA_FUNCIMPL(X) JAVA_FUNCTION_GLUE2(Impl_com_muldersoft_dynaudnorm_JDynamicAudioNormalizer_00024NativeAPI_1r, MDYNAMICAUDIONORMALIZER_CORE, X)

//JNI Headers
#include JAVA_HDRNAME(com_muldersoft_dynaudnorm_JDynamicAudioNormalizer_NativeAPI)

//PThread
#if defined(_WIN32) && defined(_MT)
#define PTW32_STATIC_LIB 1
#endif
#include <pthread.h>

//Internal
#include <Common.h>

//Globals
static pthread_mutex_t g_javaLock = PTHREAD_MUTEX_INITIALIZER;

///////////////////////////////////////////////////////////////////////////////
// Utility Functions
///////////////////////////////////////////////////////////////////////////////

#define JAVA_FIND_CLASS(VAR,NAME,RET) do \
{ \
	(VAR) = env->FindClass((NAME)); \
	if((VAR) == NULL) \
	{ \
		return (RET); \
	} \
} \
while(0)

#define JAVA_GET_METHOD(VAR,CLASS,NAME,ARGS,RET) do \
{ \
	(VAR) = env->GetMethodID((CLASS), (NAME), (ARGS)); \
	if((VAR) == NULL) \
	{ \
		return (RET); \
	} \
} \
while(0)

#define JAVA_MAP_PUT(MAP,KEY,VAL) do \
{ \
	jstring _key = env->NewStringUTF((KEY)); \
	jstring _val = env->NewStringUTF((VAL)); \
	\
	if(_key && _val) \
	{ \
		jobject _ret = env->CallObjectMethod((MAP), putMethod, _key, _val); \
		if(_ret) env->DeleteLocalRef(_ret); \
	} \
	\
	if(_key) env->DeleteLocalRef(_key); \
	if(_val) env->DeleteLocalRef(_val); \
} \
while(0)

#define JAVA_THROW_EXCEPTION(TEXT, RET) do \
{ \
	if(jclass runtimeError = env->FindClass("java/lang/Error")) \
	{ \
		env->ThrowNew(runtimeError, (TEXT)); \
		env->DeleteLocalRef(runtimeError); \
	} \
	else \
	{ \
		abort(); \
	} \
	return (RET); \
} \
while(0)

#define JAVA_TRY_CATCH(FUNC_NAME, RET, ...) \
	try \
	{ \
		return JAVA_FUNCIMPL(FUNC_NAME)(__VA_ARGS__); \
	} \
	catch(std::exception e) \
	{ \
		JAVA_THROW_EXCEPTION(e.what(), (RET)); \
	} \
	catch(...) \
	{ \
		JAVA_THROW_EXCEPTION("An unknown C++ exception has occurred!", (RET)); \
	}

///////////////////////////////////////////////////////////////////////////////
// Logging  Function
///////////////////////////////////////////////////////////////////////////////

static JavaVM *g_javaLoggingJVM = NULL;
static jobject g_javaLoggingHandler = NULL;

static jboolean javaLogMessage_Helper(JNIEnv *env, const int &level, const char *const message)
{
	jclass loggerClass = NULL;
	jmethodID logMethod = NULL;

	JAVA_FIND_CLASS(loggerClass, "com/muldersoft/dynaudnorm/JDynamicAudioNormalizer$Logger", JNI_FALSE);
	JAVA_GET_METHOD(logMethod, loggerClass, "log", "(ILjava/lang/String;)V", JNI_FALSE);

	jstring text = env->NewStringUTF(message);
	if(text)
	{
		env->CallVoidMethod(g_javaLoggingHandler, logMethod, level, text);
		env->DeleteLocalRef(text);
	}

	env->DeleteLocalRef(loggerClass);
	return JNI_TRUE;
}

static void javaLogMessage(const int logLevel, const char *const message)
{
	MY_CRITSEC_ENTER(g_javaLock);
	if(g_javaLoggingHandler && g_javaLoggingJVM)
	{
		JNIEnv *env = NULL;
		if(g_javaLoggingJVM->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK)
		{
			javaLogMessage_Helper(env, logLevel, message);
		}
	}
	MY_CRITSEC_LEAVE(g_javaLock);
}

static void javaSetLoggingHandler(JNIEnv *env, jobject loggerGlobalReference)
{
	MY_CRITSEC_ENTER(g_javaLock);
	if(g_javaLoggingHandler)
	{
		env->DeleteGlobalRef(g_javaLoggingHandler);
		g_javaLoggingHandler = NULL;
	}
	if(loggerGlobalReference)
	{
		if(env->GetJavaVM(&g_javaLoggingJVM) == 0)
		{
			g_javaLoggingHandler = loggerGlobalReference;
			MDynamicAudioNormalizer::setLogFunction(javaLogMessage);
		}
	}
	MY_CRITSEC_LEAVE(g_javaLock);
}

///////////////////////////////////////////////////////////////////////////////
// Instance Handling
///////////////////////////////////////////////////////////////////////////////

static jint g_instanceNextHandleValue = 0;
static std::map<jint, MDynamicAudioNormalizer*> g_instances;
static std::queue<jint> g_pendingHandles;

static int javaCreateHandle(MDynamicAudioNormalizer *const instance)
{
	MY_CRITSEC_ENTER(g_javaLock);
	
	jint handleValue = -1;
	if(!g_pendingHandles.empty())
	{
		handleValue = g_pendingHandles.front();
		g_pendingHandles.pop();
	}
	else if(g_instanceNextHandleValue < SHRT_MAX)
	{
		handleValue = g_instanceNextHandleValue++;
	}

	if(handleValue >= 0)
	{
		g_instances.insert(std::pair<jint, MDynamicAudioNormalizer*>(handleValue, instance));
	}

	MY_CRITSEC_LEAVE(g_javaLock);
	return handleValue;
}

static void javaCloseHandle(const jint &handleValue)
{
	MY_CRITSEC_ENTER(g_javaLock);
	
	if(g_instances.find(handleValue) != g_instances.end())
	{
		g_instances.erase(handleValue);
		g_pendingHandles.push(handleValue);
	}

	MY_CRITSEC_LEAVE(g_javaLock);
}

static MDynamicAudioNormalizer *javaHandleToInstance(const jint &handleValue)
{
	MY_CRITSEC_ENTER(g_javaLock);
	
	MDynamicAudioNormalizer *instance = NULL;
	if(g_instances.find(handleValue) != g_instances.end())
	{
		instance = g_instances[handleValue];
	}

	MY_CRITSEC_LEAVE(g_javaLock);
	return instance;
}

///////////////////////////////////////////////////////////////////////////////
// 2D Array Support
///////////////////////////////////////////////////////////////////////////////

static jboolean javaGet2DArrayElements(JNIEnv *const env, const jobjectArray outerArray, const jsize &count, double **arrayElementsOut)
{
	jclass doubleArrayClass;
	JAVA_FIND_CLASS(doubleArrayClass, "[D", JNI_FALSE);
	jboolean success = JNI_TRUE;

	for(jsize c = 0; c < count; c++)
	{
		jobject innerArray = env->GetObjectArrayElement(outerArray, jsize(c));
		if(innerArray)
		{
			if(env->IsInstanceOf(innerArray, doubleArrayClass))
			{
				if(!(arrayElementsOut[c] = env->GetDoubleArrayElements(static_cast<jdoubleArray>(innerArray), NULL)))
				{
					success = JNI_FALSE; /*failed to get the array elements*/
				}
			}
			else
			{
				success = JNI_FALSE; /*element is not a double array*/
			}
			env->DeleteLocalRef(innerArray);
		}
	}

	env->DeleteLocalRef(doubleArrayClass);
	return success;
}

static jboolean javaRelease2DArrayElements(JNIEnv *const env, jobjectArray const outerArray, const jsize &count, double *const *const arrayElements)
{
	jclass doubleArrayClass;
	JAVA_FIND_CLASS(doubleArrayClass, "[D", JNI_FALSE);
	jboolean success = JNI_TRUE;

	for(jsize c = 0; c < count; c++)
	{
		jobject innerArray = env->GetObjectArrayElement(outerArray, jsize(c));
		if(innerArray)
		{
			if(env->IsInstanceOf(innerArray, doubleArrayClass))
			{
				env->ReleaseDoubleArrayElements(static_cast<jdoubleArray>(innerArray), arrayElements[c], 0);
			}
			else
			{
				success = JNI_FALSE; /*element is not a double array*/
			}
			env->DeleteLocalRef(innerArray);
		}
	}

	env->DeleteLocalRef(doubleArrayClass);
	return success;
}

///////////////////////////////////////////////////////////////////////////////
// JNI Functions
///////////////////////////////////////////////////////////////////////////////

static jboolean JAVA_FUNCIMPL(getVersionInfo)(JNIEnv *env, jintArray versionInfo)
{
	if(versionInfo == NULL)
	{
		return JNI_FALSE;
	}

	if(env->GetArrayLength(versionInfo) == 3)
	{
		if(jint *versionInfoElements = env->GetIntArrayElements(versionInfo, NULL))
		{
			uint32_t major, minor, patch;
			MDynamicAudioNormalizer::getVersionInfo(major, minor, patch);
			
			versionInfoElements[0] = (int32_t) std::min(major, uint32_t(INT32_MAX));
			versionInfoElements[1] = (int32_t) std::min(minor, uint32_t(INT32_MAX));
			versionInfoElements[2] = (int32_t) std::min(patch, uint32_t(INT32_MAX));

			env->ReleaseIntArrayElements(versionInfo, versionInfoElements, 0);
			return JNI_TRUE;
		}
	}

	return JNI_FALSE;
}

static jboolean JAVA_FUNCIMPL(getBuildInfo)(JNIEnv *const env, jobject const buildInfo)
{
	if(buildInfo == NULL)
	{
		return JNI_FALSE;
	}

	jclass mapClass = NULL;
	jmethodID putMethod = NULL, clearMethod = NULL;

	JAVA_FIND_CLASS(mapClass, "java/util/Map", JNI_FALSE);
	JAVA_GET_METHOD(putMethod, mapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", JNI_FALSE);
	JAVA_GET_METHOD(clearMethod, mapClass, "clear", "()V", JNI_FALSE);

	if(!env->IsInstanceOf(buildInfo, mapClass))
	{
		return JNI_FALSE;
	}

	env->CallVoidMethod(buildInfo, clearMethod);

	const char *date, *time, *compiler, *arch;
	bool debug;
	MDynamicAudioNormalizer::getBuildInfo(&date, &time, &compiler, &arch, debug);

	JAVA_MAP_PUT(buildInfo, "BuildDate",    date);
	JAVA_MAP_PUT(buildInfo, "BuildTime",    time);
	JAVA_MAP_PUT(buildInfo, "Compiler",     compiler);
	JAVA_MAP_PUT(buildInfo, "Architecture", arch);
	JAVA_MAP_PUT(buildInfo, "DebugBuild",   debug ? "Yes" : "No");

	env->DeleteLocalRef(mapClass);
		
	return JNI_TRUE;
}

static jboolean JAVA_FUNCIMPL(setLoggingHandler)(JNIEnv *const env, jobject const loggerObject)
{
	if(loggerObject == NULL)
	{
		javaSetLoggingHandler(env, NULL);
		return JNI_TRUE;
	}

	jclass loggerClass = NULL;
	JAVA_FIND_CLASS(loggerClass, "com/muldersoft/dynaudnorm/JDynamicAudioNormalizer$Logger", JNI_FALSE);

	if(!env->IsInstanceOf(loggerObject, loggerClass))
	{
		return JNI_FALSE;
	}

	jobject globalReference = env->NewGlobalRef(loggerObject);
	if(globalReference)
	{
		javaSetLoggingHandler(env, globalReference);
		return JNI_TRUE;
	}

	env->DeleteLocalRef(loggerClass);
	return JNI_FALSE;
}

static jint JAVA_FUNCIMPL(createInstance)(JNIEnv *const env, const jint &channels, const jint &sampleRate, const jint &frameLenMsec, const jint &filterSize, const jdouble &peakValue, const jdouble &maxAmplification, const jdouble &targetRms, const jdouble &compressFactor, const jboolean &channelsCoupled, const jboolean &enableDCCorrection, const jboolean &altBoundaryMode)
{
	if((channels > 0) && (sampleRate > 0) && (frameLenMsec > 0) & (filterSize > 0))
	{
		MDynamicAudioNormalizer *instance = new MDynamicAudioNormalizer(channels, sampleRate, frameLenMsec, filterSize, peakValue, maxAmplification, targetRms, compressFactor, (channelsCoupled != JNI_FALSE), (enableDCCorrection != JNI_FALSE), (altBoundaryMode != JNI_FALSE));
		if(!instance->initialize())
		{
			delete instance;
			return -1;
		}

		const jint handle = javaCreateHandle(instance);
		if(handle < 0)
		{
			delete instance;
			return -1;
		}

		return handle;
	}
	return -1;
}

static jboolean JAVA_FUNCIMPL(destroyInstance)(JNIEnv *const env, const jint &handle)
{
	if(MDynamicAudioNormalizer *instance = javaHandleToInstance(handle))
	{
		javaCloseHandle(handle);
		delete instance;
		return JNI_TRUE;
	}

	return JNI_FALSE;
}

JNIEXPORT jlong JAVA_FUNCIMPL(processInplace)(JNIEnv *const env, const jint &handle, jobjectArray const samplesInOut, const jlong &inputSize)
{
	javaLogMessage(0, "processInplace() called!");

	if((handle < 0) || (samplesInOut == NULL) || (inputSize < 1))
	{
		return -1; /*invalid parameters detected*/
	}
	
	MDynamicAudioNormalizer *instance = javaHandleToInstance(handle);
	if(instance == NULL)
	{
		return -1; /*invalid handle value*/
	}

	uint32_t channels, sampleRate, frameLen, filterSize;
	if(!instance->getConfiguration(channels, sampleRate, frameLen, filterSize))
	{
		return -1; /*unable to get configuration*/
	}

	if(env->GetArrayLength(samplesInOut) < jint(channels))
	{
		return -1; /*array diemnsion is too small*/
	}

	double **arrayElements = (double**) alloca(sizeof(double*) * channels);
	if(!javaGet2DArrayElements(env, samplesInOut, channels, arrayElements))
	{
		return -1; /*failed to retrieve the array elements*/
	}

	int64_t outputSize;
	const bool success = instance->processInplace(arrayElements, inputSize, outputSize);

	if(!javaRelease2DArrayElements(env, samplesInOut, channels, arrayElements))
	{
		return -1; /*failed to release the array elements*/
	}

	return success ? outputSize : (-1);
}

///////////////////////////////////////////////////////////////////////////////
// JNI Entry Points
///////////////////////////////////////////////////////////////////////////////

extern "C"
{
	JNIEXPORT jboolean JNICALL JAVA_FUNCTION(getVersionInfo)(JNIEnv *env, jobject, jintArray versionInfo)
	{
		JAVA_TRY_CATCH(getVersionInfo, JNI_FALSE, env, versionInfo)
	}

	JNIEXPORT jboolean JNICALL JAVA_FUNCTION(getBuildInfo)(JNIEnv *env, jobject, jobject buildInfo)
	{
		JAVA_TRY_CATCH(getBuildInfo, JNI_FALSE, env, buildInfo)
	}

	JNIEXPORT jboolean JNICALL JAVA_FUNCTION(setLoggingHandler)(JNIEnv *env, jobject, jobject loggerObject)
	{
		JAVA_TRY_CATCH(setLoggingHandler, JNI_FALSE, env, loggerObject)
	}

	JNIEXPORT jint JNICALL JAVA_FUNCTION(createInstance)(JNIEnv *env, jobject, jint channels, jint sampleRate, jint frameLenMsec, jint filterSize, jdouble peakValue, jdouble maxAmplification, jdouble targetRms, jdouble compressFactor, jboolean channelsCoupled, jboolean enableDCCorrection, jboolean altBoundaryMode)
	{
		JAVA_TRY_CATCH(createInstance, -1, env, channels, sampleRate, frameLenMsec, filterSize, peakValue, maxAmplification, targetRms, compressFactor, channelsCoupled, enableDCCorrection, altBoundaryMode)
	}

	JNIEXPORT jboolean JNICALL JAVA_FUNCTION(destroyInstance)(JNIEnv *env, jobject, jint instance)
	{
		JAVA_TRY_CATCH(destroyInstance, JNI_FALSE, env, instance)
	}

	JNIEXPORT jlong JNICALL JAVA_FUNCTION(processInplace)(JNIEnv *env, jobject, jint handle, jobjectArray samplesInOut, jlong inputSize)
	{
		JAVA_TRY_CATCH(processInplace, -1, env, handle, samplesInOut, inputSize)
	}
}
