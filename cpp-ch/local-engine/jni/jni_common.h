#pragma once
#include <exception>
#include <stdexcept>
#include <jni.h>
#include <string>
#include <Common/Exception.h>
#include <Poco/Logger.h>
#include <Common/logger_useful.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace local_engine
{
jclass CreateGlobalExceptionClassReference(JNIEnv *env, const char *class_name);

jclass CreateGlobalClassReference(JNIEnv* env, const char* class_name);

jmethodID GetMethodID(JNIEnv* env, jclass this_class, const char* name, const char* sig);

jmethodID GetStaticMethodID(JNIEnv * env, jclass this_class, const char * name, const char * sig);

jstring charTojstring(JNIEnv* env, const char* pat);

jbyteArray stringTojbyteArray(JNIEnv* env, const std::string & str);

#define LOCAL_ENGINE_JNI_JMETHOD_START
#define LOCAL_ENGINE_JNI_JMETHOD_END(env) \
    if ((env)->ExceptionCheck())\
    {\
        LOG_ERROR(&Poco::Logger::get("local_engine"), "Enter java exception handle.");\
        auto excp = (env)->ExceptionOccurred();\
        (env)->ExceptionDescribe();\
        (env)->ExceptionClear();\
        jclass cls = (env)->GetObjectClass(excp); \
        jmethodID mid = env->GetMethodID(cls, "toString","()Ljava/lang/String;");\
        jstring jmsg = static_cast<jstring>((env)->CallObjectMethod(excp, mid));\
        const char *nmsg = (env)->GetStringUTFChars(jmsg, NULL);\
        std::string msg = std::string(nmsg);\
        env->ReleaseStringUTFChars(jmsg, nmsg);\
        throw DB::Exception::createRuntime(DB::ErrorCodes::LOGICAL_ERROR, msg);\
    }

template <typename ... Args>
jobject safeCallObjectMethod(JNIEnv * env, jobject obj, jmethodID method_id, Args ... args)
{
    LOCAL_ENGINE_JNI_JMETHOD_START
    auto ret = env->CallObjectMethod(obj, method_id, args...);
    LOCAL_ENGINE_JNI_JMETHOD_END(env)
    return ret;
}

template <typename ... Args>
jboolean safeCallBooleanMethod(JNIEnv * env, jobject obj, jmethodID method_id, Args ... args)
{
    LOCAL_ENGINE_JNI_JMETHOD_START
    auto ret = env->CallBooleanMethod(obj, method_id, args...);
    LOCAL_ENGINE_JNI_JMETHOD_END(env);
    return ret;
}

template <typename ... Args>
jlong safeCallLongMethod(JNIEnv * env, jobject obj, jmethodID method_id, Args ... args)
{
    LOCAL_ENGINE_JNI_JMETHOD_START
    auto ret = env->CallLongMethod(obj, method_id, args...);
    LOCAL_ENGINE_JNI_JMETHOD_END(env);
    return ret;
}

template <typename ... Args>
jint safeCallIntMethod(JNIEnv * env, jobject obj, jmethodID method_id, Args ... args)
{
    LOCAL_ENGINE_JNI_JMETHOD_START
    auto ret = env->CallIntMethod(obj, method_id, args...);
    LOCAL_ENGINE_JNI_JMETHOD_END(env);
    return ret;
}

template <typename ... Args>
void safeCallVoidMethod(JNIEnv * env, jobject obj, jmethodID method_id, Args ... args)
{
    LOCAL_ENGINE_JNI_JMETHOD_START
    env->CallVoidMethod(obj, method_id, args...);
    LOCAL_ENGINE_JNI_JMETHOD_END(env);
}
}
