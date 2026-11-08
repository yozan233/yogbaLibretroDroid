/*
 *     Copyright (C) 2021  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <jni.h>

#include <EGL/egl.h>

#include <memory>
#include <limits>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <optional>

#include "libretrodroid.h"
#include "log.h"
#include "core.h"
#include "audio.h"
#include "video.h"
#include "renderers/renderer.h"
#include "fpssync.h"
#include "input.h"
#include "rumble.h"
#include "shadermanager.h"
#include "utils/javautils.h"
#include "errorcodes.h"
#include "environment.h"
#include "renderers/es3/framebufferrenderer.h"
#include "renderers/es2/imagerendereres2.h"
#include "renderers/es3/imagerendereres3.h"
#include "utils/jnistring.h"

namespace libretrodroid {

static void dispatchSaveDataSyncedEvent(JNIEnv* env, jobject glRetroView) {
    if (!glRetroView || env->ExceptionCheck() ||
        !Environment::getInstance().isSaveDataSynced()) {
        return;
    }
    jclass cls = env->GetObjectClass(glRetroView);
    if (!cls) return;
    jmethodID method = env->GetMethodID(cls, "sendSaveDataSyncedEvent", "()V");
    if (method && !env->ExceptionCheck()) {
        env->CallVoidMethod(glRetroView, method);
        if (!env->ExceptionCheck()) {
            Environment::getInstance().resetSaveDataSynced();
        }
    }
    env->DeleteLocalRef(cls);
}

extern "C" {
#include "utils/utils.h"
#include "../../libretro-common/include/libretro.h"
#include "utils/libretrodroidexception.h"
}

extern "C" {

JNIEXPORT jint JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_availableDisks(
    JNIEnv* env,
    jclass obj
) {
    return LibretroDroid::getInstance().availableDisks();
}

JNIEXPORT jint JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_currentDisk(
    JNIEnv* env,
    jclass obj
) {
    return LibretroDroid::getInstance().currentDisk();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_changeDisk(
    JNIEnv* env,
    jclass obj,
    jint index
) {
    return LibretroDroid::getInstance().changeDisk(index);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_updateVariable(
    JNIEnv* env,
    jclass obj,
    jobject variable
) {
    Variable v = JavaUtils::variableFromJava(env, variable);
    Environment::getInstance().updateVariable(v.key, v.value);
}

JNIEXPORT jobjectArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getVariables(
    JNIEnv* env,
    jclass obj
) {
    jclass variableClass = env->FindClass("com/swordfish/libretrodroid/Variable");
    jmethodID variableMethodID = env->GetMethodID(variableClass, "<init>", "()V");

    auto variables = Environment::getInstance().getVariables();
    jobjectArray result = env->NewObjectArray(variables.size(), variableClass, nullptr);

    for (int i = 0; i < variables.size(); i++) {
        jobject jVariable = env->NewObject(variableClass, variableMethodID);

        jfieldID jKeyField = env->GetFieldID(variableClass, "key", "Ljava/lang/String;");
        jfieldID jValueField = env->GetFieldID(variableClass, "value", "Ljava/lang/String;");
        jfieldID jDescriptionField = env->GetFieldID(
            variableClass,
            "description",
            "Ljava/lang/String;"
        );

        env->SetObjectField(jVariable, jKeyField, env->NewStringUTF(variables[i].key.data()));
        env->SetObjectField(jVariable, jValueField, env->NewStringUTF(variables[i].value.data()));
        env->SetObjectField(
            jVariable,
            jDescriptionField,
            env->NewStringUTF(variables[i].description.data()));

        env->SetObjectArrayElement(result, i, jVariable);
    }
    return result;
}

JNIEXPORT jobjectArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getControllers(
    JNIEnv* env,
    jclass obj
) {
    jclass variableClass = env->FindClass("[Lcom/swordfish/libretrodroid/Controller;");

    auto controllers = Environment::getInstance().getControllers();
    jobjectArray result = env->NewObjectArray(controllers.size(), variableClass, nullptr);

    for (int i = 0; i < controllers.size(); i++) {
        jclass variableClass2 = env->FindClass("com/swordfish/libretrodroid/Controller");
        jobjectArray controllerArray = env->NewObjectArray(
            controllers[i].size(),
            variableClass2,
            nullptr
        );
        jmethodID variableMethodID = env->GetMethodID(variableClass2, "<init>", "()V");

        for (int j = 0; j < controllers[i].size(); j++) {
            jobject jController = env->NewObject(variableClass2, variableMethodID);

            jfieldID jIdField = env->GetFieldID(variableClass2, "id", "I");
            jfieldID jDescriptionField = env->GetFieldID(
                variableClass2,
                "description",
                "Ljava/lang/String;"
            );

            env->SetIntField(jController, jIdField, (int) controllers[i][j].id);
            env->SetObjectField(
                jController,
                jDescriptionField,
                env->NewStringUTF(controllers[i][j].description.data()));

            env->SetObjectArrayElement(controllerArray, j, jController);
        }

        env->SetObjectArrayElement(result, i, controllerArray);
    }
    return result;
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setControllerType(
    JNIEnv* env,
    jclass obj,
    jint port,
    jint type
) {
    LibretroDroid::getInstance().setControllerType(port, type);
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_unserializeState(
    JNIEnv* env,
    jclass obj,
    jbyteArray state
) {
    try {
        jboolean isCopy = JNI_FALSE;
        jbyte* data = env->GetByteArrayElements(state, &isCopy);
        jsize size = env->GetArrayLength(state);

        bool result = LibretroDroid::getInstance().unserializeState(data, size);
        env->ReleaseByteArrayElements(state, data, JNI_ABORT);

        return result ? JNI_TRUE : JNI_FALSE;

    } catch (std::exception &exception) {
        LOGE("Error in unserializeState: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
        return JNI_FALSE;
    }
}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_serializeState(
    JNIEnv* env,
    jclass obj
) {
    try {
        auto [data, size] = LibretroDroid::getInstance().serializeState();
        std::unique_ptr<int8_t[]> dataOwner(data);

        if (data == nullptr || size == 0){
            return nullptr;
        }

        jbyteArray result = env->NewByteArray(size);
        if (result == nullptr) {
            return nullptr;
        }
        env->SetByteArrayRegion(result, 0, size, data);

        return result;

    } catch (std::exception &exception) {
        LOGE("Error in serializeState: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
    }

    return nullptr;
}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getPixels(
    JNIEnv* env,
    jclass obj,
    jintArray outSize
) {
    jbyteArray result = nullptr;
    try {
        auto [data, sizes] = LibretroDroid::getInstance().getPixels();
        std::unique_ptr<jint[]> sizesOwner(sizes);

        if (data != nullptr && sizes != nullptr){
            size_t size = sizes[2];

            result = env->NewByteArray(size);
            if (result == nullptr) {
                return nullptr;
            }
            env->SetByteArrayRegion(result, 0, size, data);

            env->SetIntArrayRegion(outSize, 0, 5, sizes);
        }
    } catch (std::exception &exception) {
        LOGE("Error in getPixels: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
    }

    return result;
}

JNIEXPORT jint JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_readMemoryData(
        JNIEnv* env,
        jclass obj,
        jint offset,
        jbyteArray buffer
) {
    unsigned length = env->GetArrayLength(buffer);

    try {
        auto [data, size] = LibretroDroid::getInstance().getMemoryData(offset, length);

        std::unique_ptr<int8_t[]> ownedData(data);
        if (length == 0) {
            return static_cast<jint>(size);
        }

        if (data == nullptr || size == 0) {
            return -1;
        }

        env->SetByteArrayRegion(buffer, 0, size, data);

        return size;
    } catch (std::exception &exception) {
        LOGE("Error in readMemoryData: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
    }

    return -1;
}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_copyUnpatchedRom(
    JNIEnv* env, jclass obj
) {
    try {
        auto& droid = LibretroDroid::getInstance();
        const size_t size = droid.copyUnpatchedRom(nullptr, 0);
        if (size < 0xC0 || size > 32 * 1024 * 1024) return nullptr;
        auto result = env->NewByteArray(static_cast<jsize>(size));
        if (!result) return nullptr;
        jbyte* bytes = env->GetByteArrayElements(result, nullptr);
        if (!bytes) return nullptr;
        const bool copied = droid.copyUnpatchedRom(bytes, size) == size;
        env->ReleaseByteArrayElements(result, bytes, copied ? 0 : JNI_ABORT);
        return copied ? result : nullptr;
    } catch (const std::exception& exception) {
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
        return nullptr;
    }
}

JNIEXPORT jlong JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getBiosIdentity(
    JNIEnv* env, jclass obj
) {
    return static_cast<jlong>(LibretroDroid::getInstance().getBiosIdentity());
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_writeMemoryData(
        JNIEnv* env,
        jclass obj,
        jint offset,
        jbyteArray buffer
) {
    unsigned length = env->GetArrayLength(buffer);

    try {
        jboolean isCopy = JNI_FALSE;
        jbyte* data = env->GetByteArrayElements(buffer, &isCopy);

        LibretroDroid::getInstance().setMemoryData(offset, data, length);

        env->ReleaseByteArrayElements(buffer, data, JNI_ABORT);

    } catch (std::exception &exception) {
        LOGE("Error in writeMemoryData: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
    }

}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setCheat(
    JNIEnv* env,
    jclass obj,
    jint index,
    jboolean enabled,
    jstring code
) {
    try {
        auto codeString = JniString(env, code);
        LibretroDroid::getInstance().setCheat(index, enabled, codeString.stdString());
    } catch (std::exception &exception) {
        LOGE("Error in setCheat: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_CHEAT);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_resetCheat(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().resetCheat();
    } catch (std::exception &exception) {
        LOGE("Error in resetCheat: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_CHEAT);
    }
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_unserializeSRAM(
    JNIEnv* env,
    jclass obj,
    jbyteArray sram
) {
    if (!sram) return JNI_FALSE;
    try {
        jsize size = env->GetArrayLength(sram);
        if (size <= 0 || env->ExceptionCheck()) return JNI_FALSE;
        std::vector<int8_t> data(static_cast<size_t>(size));
        env->GetByteArrayRegion(sram, 0, size, reinterpret_cast<jbyte*>(data.data()));
        if (env->ExceptionCheck()) return JNI_FALSE;
        return LibretroDroid::getInstance().unserializeSRAM(data.data(), data.size())
            ? JNI_TRUE : JNI_FALSE;

    } catch (std::exception &exception) {
        LOGE("Error in unserializeSRAM: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
        return JNI_FALSE;
    }

}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_serializeSRAM(
    JNIEnv* env,
    jclass obj
) {
    try {
        auto [data, size] = LibretroDroid::getInstance().serializeSRAM();
        std::unique_ptr<int8_t[]> dataOwner(data);
        if (!data || !size || size > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
            return nullptr;
        }

        jbyteArray result = env->NewByteArray(static_cast<jsize>(size));
        if (!result) return nullptr;
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(size), reinterpret_cast<jbyte*>(data));
        if (env->ExceptionCheck()) return nullptr;

        return result;

    } catch (std::exception &exception) {
        LOGE("Error in serializeSRAM: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
    }

    return nullptr;
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_reset(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().reset();
    } catch (std::exception &exception) {
        LOGE("Error in clear: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_GENERIC);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onSurfaceChanged(
    JNIEnv* env,
    jclass obj,
    jint width,
    jint height
) {
    LibretroDroid::getInstance().onSurfaceChanged(width, height);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onSurfaceCreated(
    JNIEnv* env,
    jclass obj
) {
    LibretroDroid::getInstance().onSurfaceCreated();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onMotionEvent(
    JNIEnv* env,
    jclass obj,
    jint port,
    jint source,
    jfloat xAxis,
    jfloat yAxis
) {
    LibretroDroid::getInstance().onMotionEvent(port, source, xAxis, yAxis);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onTouchEvent(
    JNIEnv* env,
    jclass obj,
    jfloat xAxis,
    jfloat yAxis
) {
    LibretroDroid::getInstance().onTouchEvent(xAxis, yAxis);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_onKeyEvent(
    JNIEnv* env,
    jclass obj,
    jint port,
    jint action,
    jint keyCode
) {
    LibretroDroid::getInstance().onKeyEvent(port, action, keyCode);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_create(
    JNIEnv* env,
    jclass obj,
    jint GLESVersion,
    jstring soFilePath,
    jstring systemDir,
    jstring savesDir,
    jobjectArray jVariables,
    jobject shaderConfig,
    jfloat refreshRate,
    jboolean preferLowLatencyAudio,
    jboolean enableVirtualFileSystem,
    jboolean enableMicrophone,
    jboolean skipDuplicateFrames,
    jobject immersiveMode,
    jstring language
) {
    try {
        auto corePath = JniString(env, soFilePath);
        auto deviceLanguage = JniString(env, language);
        auto systemDirectory = JniString(env, systemDir);
        auto savesDirectory = JniString(env, savesDir);

        std::vector<Variable> variables;
        int size = env->GetArrayLength(jVariables);
        for (int i = 0; i < size; i++) {
            auto jVariable = (jobject) env->GetObjectArrayElement(jVariables, i);
            auto variable = JavaUtils::variableFromJava(env, jVariable);
            variables.push_back(variable);
        }

        std::optional<ImmersiveMode::Config> parsedConfig = std::nullopt;
        if (immersiveMode != nullptr) {
            jclass configClass = env->GetObjectClass(immersiveMode);
            jfieldID downscaledWidthField = env->GetFieldID(configClass, "downscaledWidth", "I");
            jfieldID downscaledHeightField = env->GetFieldID(configClass, "downscaledHeight", "I");
            jfieldID blurMaskSizeField = env->GetFieldID(configClass, "blurMaskSize", "I");
            jfieldID blurBrightnessField = env->GetFieldID(configClass, "blurBrightness", "F");
            jfieldID blurSkipUpdateField = env->GetFieldID(configClass, "blurSkipUpdate", "I");
            jfieldID blendFactorField = env->GetFieldID(configClass, "blendFactor", "F");

            ImmersiveMode::Config config {};
            config.downscaledWidth = env->GetIntField(immersiveMode, downscaledWidthField);
            config.downscaledHeight = env->GetIntField(immersiveMode, downscaledHeightField);
            config.blurMaskSize = env->GetIntField(immersiveMode, blurMaskSizeField);
            config.blurBrightness = env->GetFloatField(immersiveMode, blurBrightnessField);
            config.blurSkipUpdate = env->GetIntField(immersiveMode, blurSkipUpdateField);
            config.blendFactor = env->GetFloatField(immersiveMode, blendFactorField);
            parsedConfig = config;
        }

        LibretroDroid::getInstance().create(
            GLESVersion,
            corePath.stdString(),
            systemDirectory.stdString(),
            savesDirectory.stdString(),
            variables,
            JavaUtils::shaderFromJava(env, shaderConfig),
            refreshRate,
            preferLowLatencyAudio,
            enableVirtualFileSystem,
            enableMicrophone,
            skipDuplicateFrames,
            parsedConfig,
            deviceLanguage.stdString()
        );

    } catch (libretrodroid::LibretroDroidError& exception) {
        LOGE("Error in create: %s", exception.what());
        JavaUtils::throwRetroException(env, exception.getErrorCode());
    } catch (std::exception &exception) {
        LOGE("Error in create: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_LIBRARY);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_loadGameFromPath(
    JNIEnv* env,
    jclass obj,
    jstring gameFilePath
) {
    auto gamePath = JniString(env, gameFilePath);

    try {
        LibretroDroid::getInstance().loadGameFromPath(gamePath.stdString());
    } catch (std::exception &exception) {
        LOGE("Error in loadGameFromPath: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_loadGameFromBytes(
    JNIEnv* env,
    jclass obj,
    jbyteArray gameFileBytes
) {
    if (!gameFileBytes) {
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
        return;
    }
    try {
        const jsize size = env->GetArrayLength(gameFileBytes);
        if (size <= 0) {
            JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
            return;
        }
        std::vector<int8_t> data(static_cast<size_t>(size));
        env->GetByteArrayRegion(gameFileBytes, 0, size,
            reinterpret_cast<jbyte*>(data.data()));
        if (env->ExceptionCheck()) {
            return;
        }
        LibretroDroid::getInstance().loadGameFromBytes(data.data(), data.size());
    } catch (std::exception &exception) {
        LOGE("Error in loadGameFromBytes: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_loadGameFromVirtualFiles(
        JNIEnv* env,
        jclass obj,
        jobject virtualFileList
) {

    try {
        jmethodID getVirtualFileMethodID = env->GetMethodID(
                env->FindClass("com/swordfish/libretrodroid/DetachedVirtualFile"),
                "getVirtualPath",
                "()Ljava/lang/String;"
        );
        jmethodID getFileDescriptorMethodID = env->GetMethodID(
                env->FindClass("com/swordfish/libretrodroid/DetachedVirtualFile"),
                "getFileDescriptor",
                "()I"
        );

        std::vector<VFSFile> virtualFiles;

        JavaUtils::forEachOnJavaIterable(env, virtualFileList, [&](jobject item) {
            JniString virtualFileName(env,(jstring) env->CallObjectMethod(
                item,
                getVirtualFileMethodID
            ));

            int fileDescriptor = env->CallIntMethod(item, getFileDescriptorMethodID);
            virtualFiles.emplace_back(VFSFile(virtualFileName.stdString(), fileDescriptor));
        });

        LibretroDroid::getInstance().loadGameFromVirtualFiles(std::move(virtualFiles));
    } catch (std::exception &exception) {
        LOGE("Error in loadGameFromDescriptors: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_destroy(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().destroy();
    } catch (std::exception &exception) {
        LOGE("Error in destroy: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_GENERIC);
    }
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_isRunning(
        JNIEnv* env,
        jclass obj
) {
    return LibretroDroid::getInstance().isRunning();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_resume(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().resume();
    } catch (std::exception &exception) {
        LOGE("Error in resume: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_GENERIC);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_pause(
    JNIEnv* env,
    jclass obj
) {
    try {
        LibretroDroid::getInstance().pause();
    } catch (std::exception &exception) {
        LOGE("Error in pause: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_GENERIC);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_step(
    JNIEnv* env,
    jclass obj,
    jobject glRetroView
) {
    LibretroDroid::getInstance().step();

    if (LibretroDroid::getInstance().requiresVideoRefresh()) {
        LibretroDroid::getInstance().clearRequiresVideoRefresh();
        jclass cls = env->GetObjectClass(glRetroView);
        jmethodID requestAspectRatioUpdate = env->GetMethodID(cls, "refreshAspectRatio", "()V");
        env->CallVoidMethod(glRetroView, requestAspectRatioUpdate);
    }

    if (LibretroDroid::getInstance().isRumbleEnabled()) {
        LibretroDroid::getInstance().handleRumbleUpdates([&](int port, float weak, float strong) {
            jclass cls = env->GetObjectClass(glRetroView);
            jmethodID sendRumbleStrengthMethodID = env->GetMethodID(cls, "sendRumbleEvent", "(IFF)V");
            env->CallVoidMethod(glRetroView, sendRumbleStrengthMethodID, port, weak, strong);
        });
    }

    dispatchSaveDataSyncedEvent(env, glRetroView);

}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_glJniEnvInit(
        JNIEnv* env,
        jclass obj
) {
    LibretroDroid::getInstance().glJniEnvInit(env);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_glJniEnvDestroy(
        JNIEnv* env,
        jclass obj
) {
    LibretroDroid::getInstance().glJniEnvDestroy();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setRumbleEnabled(
    JNIEnv* env,
    jclass obj,
    jboolean enabled
) {
    LibretroDroid::getInstance().setRumbleEnabled(enabled);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setFrameSpeed(
    JNIEnv* env,
    jclass obj,
    jfloat speed
) {
    LibretroDroid::getInstance().setFrameSpeed(speed);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setAudioEnabled(
    JNIEnv* env,
    jclass obj,
    jboolean enabled
) {
    LibretroDroid::getInstance().setAudioEnabled(enabled);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setAudioVolume(
        JNIEnv* env,
        jclass obj,
        jint volume
) {
    LibretroDroid::getInstance().setAudioVolume(volume);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setShaderConfig(
    JNIEnv* env,
    jclass obj,
    jobject shaderConfig
) {
    LibretroDroid::getInstance().setShaderConfig(JavaUtils::shaderFromJava(env, shaderConfig));
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setViewport(
    JNIEnv* env,
    jclass obj,
    jfloat x,
    jfloat y,
    jfloat width,
    jfloat height
) {
    LibretroDroid::getInstance().setViewport(Rect(x, y, width, height));
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_refreshAspectRatio(
    JNIEnv* env,
    jclass obj
) {
    LibretroDroid::getInstance().refreshAspectRatio();
}

JNIEXPORT jfloat JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getAspectRatio(
        JNIEnv* env,
        jclass obj
) {
    return LibretroDroid::getInstance().getAspectRatio();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_reloadGameFromBytes(
    JNIEnv* env,
    jclass obj,
    jbyteArray gameFileBytes
) {
    if (!gameFileBytes) {
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
        return;
    }
    try {
        const jsize size = env->GetArrayLength(gameFileBytes);
        if (size <= 0) {
            JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
            return;
        }
        std::vector<int8_t> data(static_cast<size_t>(size));
        env->GetByteArrayRegion(gameFileBytes, 0, size,
            reinterpret_cast<jbyte*>(data.data()));
        if (env->ExceptionCheck()) {
            return;
        }
        LibretroDroid::getInstance().reloadGameFromBytes(
            data.data(), data.size());
    } catch (std::exception &exception) {
        LOGE("Error in reloadGameFromBytes: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_reloadGameFromBytesWithSystemDirectory(
    JNIEnv* env,
    jclass obj,
    jbyteArray gameFileBytes,
    jstring systemDirectory
) {
    if (!systemDirectory) {
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
        return;
    }
    try {
        JniString directory(env, systemDirectory);
        if (env->ExceptionCheck()) return;
        auto& environment = Environment::getInstance();
        struct RestoreDirectory {
            Environment& environment;
            std::string previous;
            ~RestoreDirectory() {
                environment.exchangeSystemDirectory(std::move(previous));
            }
        } restore { environment, environment.exchangeSystemDirectory(directory.stdString()) };
        // Restore even when the load reports a pending Java exception.
        Java_com_swordfish_libretrodroid_LibretroDroid_reloadGameFromBytes(env, obj, gameFileBytes);
    } catch (const std::exception&) {
        JavaUtils::throwRetroException(env, ERROR_LOAD_GAME);
    }
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_stepWithoutVideo(
    JNIEnv* env,
    jclass obj,
    jobject glRetroView
) {
    LibretroDroid::getInstance().stepWithoutVideo();
    dispatchSaveDataSyncedEvent(env, glRetroView);
}

JNIEXPORT jfloat JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getContentFrameRate(
        JNIEnv* env,
        jclass obj
) {
    return LibretroDroid::getInstance().getContentFrameRate();
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setAspectRatio(
        JNIEnv* env,
        jclass obj,
        jfloat aspectRatio
) {
    LibretroDroid::getInstance().setAspectRatio(aspectRatio);
}

JNIEXPORT jobject JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getCoreInfo(
        JNIEnv* env,
        jclass obj
) {
    struct retro_system_info systemInfo {};
    LibretroDroid::getInstance().getSystemInfo(&systemInfo);

    jclass coreInfoClass = env->FindClass("com/swordfish/libretrodroid/CoreInfo");
    jmethodID coreInfoMethodID = env->GetMethodID(coreInfoClass, "<init>", "()V");

    jobject jCoreInfo = env->NewObject(coreInfoClass, coreInfoMethodID);

    jfieldID jNameField = env->GetFieldID(coreInfoClass, "name", "Ljava/lang/String;");
    jfieldID jVersionField = env->GetFieldID(coreInfoClass, "version", "Ljava/lang/String;");
    jfieldID jTypesField = env->GetFieldID(coreInfoClass, "types", "Ljava/lang/String;");
    jfieldID jNeedFullPathField = env->GetFieldID(coreInfoClass, "needFullPath", "Z");
    jfieldID jBlockExtractField = env->GetFieldID(coreInfoClass, "blockExtract", "Z");

    env->SetObjectField(jCoreInfo, jNameField, env->NewStringUTF(systemInfo.library_name));
    env->SetObjectField(jCoreInfo, jVersionField, env->NewStringUTF(systemInfo.library_version));
    env->SetObjectField(jCoreInfo, jTypesField, env->NewStringUTF(systemInfo.valid_extensions));
    env->SetBooleanField(jCoreInfo, jNeedFullPathField, systemInfo.need_fullpath ? JNI_TRUE : JNI_FALSE);
    env->SetBooleanField(jCoreInfo, jBlockExtractField, systemInfo.block_extract ? JNI_TRUE : JNI_FALSE);

    return jCoreInfo;
}

//void retro_set_rtc(unsigned type,int64_t value);
JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setRTC(
        JNIEnv* env,
        jclass obj,
        jint type,
        jlong value
) {
    LibretroDroid::getInstance().setRTC(type, value);
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setMultiplayer(
        JNIEnv* env,
        jclass obj,
        jint playerId,
        jint playerCount
) {
    LibretroDroid::getInstance().setMultiplayer(playerId, playerCount);
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_configureLinkNetplay(
        JNIEnv* env,
        jclass obj,
        jint mode,
        jint inputDelayFrames
) {
    return LibretroDroid::getInstance().configureLinkNetplay(mode, inputDelayFrames)
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getLinkNetplayStatus(
        JNIEnv* env,
        jclass obj
) {
    return static_cast<jint>(LibretroDroid::getInstance().getLinkNetplayStatus());
}

JNIEXPORT jlong JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getRomCrc32(
        JNIEnv* env,
        jclass obj
) {
    return static_cast<jlong>(LibretroDroid::getInstance().getRomCrc32());
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setMultiplayerSockets(
        JNIEnv* env,
        jclass obj,
        jintArray socketFds,
        jint playerId,
        jint playerCount
) {
    if (!socketFds || env->GetArrayLength(socketFds) != 4) {
        return JNI_FALSE;
    }
    jint values[4] = { -1, -1, -1, -1 };
    env->GetIntArrayRegion(socketFds, 0, 4, values);
    int nativeFds[4] = { values[0], values[1], values[2], values[3] };
    return LibretroDroid::getInstance().setMultiplayerSockets(nativeFds, playerId, playerCount)
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_isNativeLinkActive(
        JNIEnv* env,
        jclass obj
) {
    return LibretroDroid::getInstance().isNativeLinkActive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_linkRemoteStart(
        JNIEnv* env,
        jclass obj,
        jint sequence,
        jint mode,
        jint transferCycles,
        jint primaryData
) {
    return LibretroDroid::getInstance().linkRemoteStart(
        sequence, mode, transferCycles, static_cast<uint32_t>(primaryData));
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_linkRemoteMode(
        JNIEnv* env,
        jclass obj,
        jint playerId,
        jint mode
) {
    return LibretroDroid::getInstance().linkRemoteMode(playerId, mode) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_linkRemoteCommit(
        JNIEnv* env,
        jclass obj,
        jint sequence,
        jintArray data
) {
    if (!data || env->GetArrayLength(data) != 4) {
        return JNI_FALSE;
    }
    jint values[4] = {};
    env->GetIntArrayRegion(data, 0, 4, values);
    return LibretroDroid::getInstance().linkRemoteCommit(
        sequence, reinterpret_cast<const uint32_t*>(values)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_linkCancel(
        JNIEnv* env,
        jclass obj
) {
    LibretroDroid::getInstance().linkCancel();
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_hasLinkSupport(
        JNIEnv* env,
        jclass obj
) {
    return LibretroDroid::getInstance().hasLinkSupport() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_hasRomConsoleSupport(
        JNIEnv* env,
        jclass obj
) {
    return LibretroDroid::getInstance().hasRomConsoleSupport() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getPrimaryRomConsoleHandle(
        JNIEnv* env,
        jclass obj
) {
    return static_cast<jlong>(LibretroDroid::getInstance().getPrimaryRomConsole());
}

JNIEXPORT jlong JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_createRomConsoleHandle(
        JNIEnv* env,
        jclass obj,
        jbyteArray romBytes,
        jbyteArray saveRam
) {
    if (!romBytes) {
        return 0;
    }
    jsize romSize = env->GetArrayLength(romBytes);
    if (romSize <= 0) {
        return 0;
    }
    std::vector<int8_t> rom(static_cast<size_t>(romSize));
    env->GetByteArrayRegion(romBytes, 0, romSize,
                            reinterpret_cast<jbyte*>(rom.data()));
    if (env->ExceptionCheck()) {
        return 0;
    }

    std::vector<int8_t> sram;
    jsize sramSize = 0;
    if (saveRam) {
        sramSize = env->GetArrayLength(saveRam);
        if (sramSize > 0) {
            sram.resize(static_cast<size_t>(sramSize));
            env->GetByteArrayRegion(saveRam, 0, sramSize,
                                    reinterpret_cast<jbyte*>(sram.data()));
            if (env->ExceptionCheck()) {
                return 0;
            }
        }
    }
    jlong handle = static_cast<jlong>(LibretroDroid::getInstance().createRomConsole(
        rom.data(), rom.size(),
        sram.empty() ? nullptr : sram.data(), sram.size()));
    return handle;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_destroyRomConsoleHandle(
        JNIEnv* env,
        jclass obj,
        jlong handle
) {
    return LibretroDroid::getInstance().destroyRomConsole(
        static_cast<uint64_t>(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getRomConsoleCrc32(
        JNIEnv* env,
        jclass obj,
        jlong handle
) {
    return static_cast<jlong>(LibretroDroid::getInstance().getRomConsoleCrc32(
        static_cast<uint64_t>(handle)));
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_startRomConsoleLanLink(
        JNIEnv* env,
        jclass obj,
        jintArray socketFds,
        jlongArray handles,
        jint localPlayerId
) {
    if (!socketFds || env->GetArrayLength(socketFds) < 4 || !handles) {
        return JNI_FALSE;
    }
    jsize count = env->GetArrayLength(handles);
    if (count < 2 || count > 4 || localPlayerId < 0 || localPlayerId >= count) {
        return JNI_FALSE;
    }
    jint javaFds[4] = { -1, -1, -1, -1 };
    env->GetIntArrayRegion(socketFds, 0, 4, javaFds);
    int nativeFds[4] = { javaFds[0], javaFds[1], javaFds[2], javaFds[3] };
    std::vector<jlong> javaHandles(static_cast<size_t>(count));
    env->GetLongArrayRegion(handles, 0, count, javaHandles.data());
    std::vector<uint64_t> nativeHandles(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        nativeHandles[static_cast<size_t>(i)] = static_cast<uint64_t>(javaHandles[i]);
    }
    return LibretroDroid::getInstance().startLanLink(nativeFds, nativeHandles,
        static_cast<unsigned>(localPlayerId)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_startLocalLink(
        JNIEnv* env,
        jclass obj,
        jlongArray handles
) {
    if (!handles) {
        return JNI_FALSE;
    }
    jsize count = env->GetArrayLength(handles);
    if (count < 2 || count > 4) {
        return JNI_FALSE;
    }
    std::vector<jlong> javaHandles(static_cast<size_t>(count));
    env->GetLongArrayRegion(handles, 0, count, javaHandles.data());
    std::vector<uint64_t> nativeHandles(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        nativeHandles[static_cast<size_t>(i)] = static_cast<uint64_t>(javaHandles[i]);
    }
    return LibretroDroid::getInstance().startLocalLink(nativeHandles)
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setForegroundRomConsole(
        JNIEnv* env,
        jclass obj,
        jlong handle
) {
    return LibretroDroid::getInstance().setForegroundRomConsole(
        static_cast<uint64_t>(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getForegroundRomConsoleHandle(
        JNIEnv* env,
        jclass obj
) {
    return static_cast<jlong>(LibretroDroid::getInstance().getForegroundRomConsole());
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_resetRomConsole(
        JNIEnv* env,
        jclass obj,
        jlong handle
) {
    return LibretroDroid::getInstance().resetRomConsole(
        static_cast<uint64_t>(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_getRomConsolePixels(
        JNIEnv* env,
        jclass obj,
        jlong handle,
        jintArray outSize
) {
    jbyteArray result = nullptr;
    try {
        auto [data, sizes] = LibretroDroid::getInstance().getRomConsolePixels(
            static_cast<uint64_t>(handle));
        std::unique_ptr<jint[]> sizesOwner(sizes);
        if (data && sizes) {
            const size_t size = sizes[2];
            result = env->NewByteArray(size);
            if (!result) {
                return nullptr;
            }
            env->SetByteArrayRegion(result, 0, size, data);
            env->SetIntArrayRegion(outSize, 0, 5, sizes);
        }
    } catch (std::exception& exception) {
        LOGE("Error in getRomConsolePixels: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
    }
    return result;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_resetRomConsoleCheat(
        JNIEnv* env,
        jclass obj,
        jlong handle
) {
    return LibretroDroid::getInstance().resetRomConsoleCheat(
        static_cast<uint64_t>(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_setRomConsoleCheat(
        JNIEnv* env,
        jclass obj,
        jlong handle,
        jint type,
        jboolean enabled,
        jstring code
) {
    if (!code) {
        return JNI_FALSE;
    }
    try {
        auto codeString = JniString(env, code);
        return LibretroDroid::getInstance().setRomConsoleCheat(
            static_cast<uint64_t>(handle), static_cast<unsigned>(type),
            enabled == JNI_TRUE, codeString.stdString()) ? JNI_TRUE : JNI_FALSE;
    } catch (std::exception &exception) {
        LOGE("Error in setRomConsoleCheat: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_CHEAT);
        return JNI_FALSE;
    }
}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_serializeRomConsoleSRAM(
        JNIEnv* env,
        jclass obj,
        jlong handle
) {
    try {
        auto [data, size] = LibretroDroid::getInstance().serializeRomConsoleSRAM(
            static_cast<uint64_t>(handle));
        std::unique_ptr<int8_t[]> dataOwner(data);
        if (!data || !size || size > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
            return nullptr;
        }
        jbyteArray result = env->NewByteArray(static_cast<jsize>(size));
        if (result) {
            env->SetByteArrayRegion(result, 0, static_cast<jsize>(size),
                                    reinterpret_cast<jbyte*>(data));
        }
        return env->ExceptionCheck() ? nullptr : result;
    } catch (const std::exception& exception) {
        LOGE("Error in serializeRomConsoleSRAM: %s", exception.what());
        JavaUtils::throwRetroException(env, ERROR_SERIALIZATION);
        return nullptr;
    }
}

JNIEXPORT jbyteArray JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_serializeRomConsoleState(
        JNIEnv* env,
        jclass obj,
        jlong handle
) {
    auto [data, size] = LibretroDroid::getInstance().serializeRomConsoleState(
        static_cast<uint64_t>(handle));
    if (!data || !size) {
        delete[] data;
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(static_cast<jsize>(size));
    if (result) {
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(size),
                                reinterpret_cast<jbyte*>(data));
    }
    delete[] data;
    return result;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_unserializeRomConsoleState(
        JNIEnv* env,
        jclass obj,
        jlong handle,
        jbyteArray state
) {
    if (!state) {
        return JNI_FALSE;
    }
    jsize size = env->GetArrayLength(state);
    if (size <= 0) {
        return JNI_FALSE;
    }
    std::vector<int8_t> data(static_cast<size_t>(size));
    env->GetByteArrayRegion(state, 0, size, reinterpret_cast<jbyte*>(data.data()));
    return LibretroDroid::getInstance().unserializeRomConsoleState(
        static_cast<uint64_t>(handle), data.data(), data.size())
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_unserializeLanPlayerState(
        JNIEnv* env,
        jclass obj,
        jint playerId,
        jbyteArray state,
        jint generation) {
    //UNUSED(obj);
    if (!state || playerId < 0 || generation <= 0) {
        return JNI_FALSE;
    }
    jsize size = env->GetArrayLength(state);
    if (size <= 0) {
        return JNI_FALSE;
    }
    std::vector<int8_t> data(static_cast<size_t>(size));
    env->GetByteArrayRegion(state, 0, size, reinterpret_cast<jbyte*>(data.data()));
    return LibretroDroid::getInstance().unserializeLanPlayerState(
        static_cast<unsigned>(playerId), data.data(), data.size(),
        static_cast<uint32_t>(generation)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_resetLanPlayer(
        JNIEnv* env,
        jclass obj,
        jint playerId,
        jint generation) {
    //UNUSED(env);
    //UNUSED(obj);
    if (playerId < 0 || generation <= 0) {
        return JNI_FALSE;
    }
    return LibretroDroid::getInstance().resetLanPlayer(
        static_cast<unsigned>(playerId), static_cast<uint32_t>(generation))
        ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_pollLanStateBarrier(
        JNIEnv* env, jclass obj, jint generation) {
    return generation > 0 ? LibretroDroid::getInstance().pollLanStateBarrier(
        static_cast<uint32_t>(generation)) : -1;
}

JNIEXPORT jboolean JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_unserializeLanResetState(
        JNIEnv* env, jclass obj, jint playerId, jbyteArray state, jint generation) {
    if (!state || playerId < 0 || generation <= 0) return JNI_FALSE;
    jsize size = env->GetArrayLength(state);
    if (size <= 0) return JNI_FALSE;
    std::vector<int8_t> data(static_cast<size_t>(size));
    env->GetByteArrayRegion(state, 0, size, reinterpret_cast<jbyte*>(data.data()));
    if (env->ExceptionCheck()) return JNI_FALSE;
    return LibretroDroid::getInstance().unserializeLanResetState(
        static_cast<unsigned>(playerId), data.data(), data.size(),
        static_cast<uint32_t>(generation)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_swordfish_libretrodroid_LibretroDroid_stopLocalLink(
        JNIEnv* env,
        jclass obj
) {
    LibretroDroid::getInstance().stopLocalLink();
}
}}
