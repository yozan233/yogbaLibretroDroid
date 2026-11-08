/*
 *     Copyright (C) 2019  Filippo Scognamiglio
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

package com.swordfish.libretrodroid;

import java.util.List;

public class LibretroDroid {

    static {
        System.loadLibrary("libretrodroid");
    }

    public static final int MOTION_SOURCE_DPAD = 0;
    public static final int MOTION_SOURCE_ANALOG_LEFT = 1;
    public static final int MOTION_SOURCE_ANALOG_RIGHT = 2;
    public static final int MOTION_SOURCE_POINTER = 3;

    public static final int SHADER_DEFAULT = 0;
    public static final int SHADER_CRT = 1;
    public static final int SHADER_LCD = 2;
    public static final int SHADER_SHARP = 3;
    public static final int SHADER_UPSCALE_CUT = 4;
    public static final int SHADER_UPSCALE_CUT2 = 5;
    public static final int SHADER_UPSCALE_CUT3 = 6;
    public static final int SHADER_CUSTOM = 7;

    public static final String SHADER_UPSCALE_CUT_PARAM_USE_DYNAMIC_BLEND = "USE_DYNAMIC_BLEND";
    public static final String SHADER_UPSCALE_CUT_PARAM_BLEND_MIN_CONTRAST_EDGE = "BLEND_MIN_CONTRAST_EDGE";
    public static final String SHADER_UPSCALE_CUT_PARAM_BLEND_MAX_CONTRAST_EDGE = "BLEND_MAX_CONTRAST_EDGE";
    public static final String SHADER_UPSCALE_CUT_PARAM_BLEND_MIN_SHARPNESS = "BLEND_MIN_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT_PARAM_BLEND_MAX_SHARPNESS = "BLEND_MAX_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT_PARAM_STATIC_BLEND_SHARPNESS = "STATIC_BLEND_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT_PARAM_EDGE_USE_FAST_LUMA = "EDGE_USE_FAST_LUMA";
    public static final String SHADER_UPSCALE_CUT_PARAM_EDGE_MIN_VALUE = "EDGE_MIN_VALUE";
    public static final String SHADER_UPSCALE_CUT_PARAM_EDGE_MIN_CONTRAST = "EDGE_MIN_CONTRAST";

    public static final String SHADER_UPSCALE_CUT2_PARAM_USE_DYNAMIC_BLEND = "USE_DYNAMIC_BLEND";
    public static final String SHADER_UPSCALE_CUT2_PARAM_BLEND_MIN_CONTRAST_EDGE = "BLEND_MIN_CONTRAST_EDGE";
    public static final String SHADER_UPSCALE_CUT2_PARAM_BLEND_MAX_CONTRAST_EDGE = "BLEND_MAX_CONTRAST_EDGE";
    public static final String SHADER_UPSCALE_CUT2_PARAM_BLEND_MIN_SHARPNESS = "BLEND_MIN_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT2_PARAM_BLEND_MAX_SHARPNESS = "BLEND_MAX_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT2_PARAM_STATIC_BLEND_SHARPNESS = "STATIC_BLEND_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT2_PARAM_EDGE_USE_FAST_LUMA = "EDGE_USE_FAST_LUMA";
    public static final String SHADER_UPSCALE_CUT2_PARAM_SOFT_EDGES_SHARPENING = "SOFT_EDGES_SHARPENING";
    public static final String SHADER_UPSCALE_CUT2_PARAM_SOFT_EDGES_SHARPENING_AMOUNT = "SOFT_EDGES_SHARPENING_AMOUNT";
    public static final String SHADER_UPSCALE_CUT2_PARAM_HARD_EDGES_SEARCH_MAX_ERROR = "HARD_EDGES_SEARCH_MAX_ERROR";

    public static final String SHADER_UPSCALE_CUT3_PARAM_USE_DYNAMIC_BLEND = "USE_DYNAMIC_BLEND";
    public static final String SHADER_UPSCALE_CUT3_PARAM_BLEND_MIN_CONTRAST_EDGE = "BLEND_MIN_CONTRAST_EDGE";
    public static final String SHADER_UPSCALE_CUT3_PARAM_BLEND_MAX_CONTRAST_EDGE = "BLEND_MAX_CONTRAST_EDGE";
    public static final String SHADER_UPSCALE_CUT3_PARAM_BLEND_MIN_SHARPNESS = "BLEND_MIN_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT3_PARAM_BLEND_MAX_SHARPNESS = "BLEND_MAX_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT3_PARAM_STATIC_BLEND_SHARPNESS = "STATIC_BLEND_SHARPNESS";
    public static final String SHADER_UPSCALE_CUT3_PARAM_EDGE_USE_FAST_LUMA = "EDGE_USE_FAST_LUMA";
    public static final String SHADER_UPSCALE_CUT3_PARAM_SOFT_EDGES_SHARPENING = "SOFT_EDGES_SHARPENING";
    public static final String SHADER_UPSCALE_CUT3_PARAM_SOFT_EDGES_SHARPENING_AMOUNT = "SOFT_EDGES_SHARPENING_AMOUNT";
    public static final String SHADER_UPSCALE_CUT3_PARAM_HARD_EDGES_SEARCH_MAX_ERROR = "HARD_EDGES_SEARCH_MAX_ERROR";
    public static final String SHADER_UPSCALE_CUT3_PARAM_HARD_EDGES_SEARCH_MAX_DISTANCE = "HARD_EDGES_SEARCH_MAX_DISTANCE";

    public static final String SHADER_CUSTOM_PARAM_VERTEX = "VERTEX";
    public static final String SHADER_CUSTOM_PARAM_FRAGMENT = "FRAGMENT";
    public static final String SHADER_CUSTOM_PARAM_LINEAR_TEXTURE = "LINEAR_TEXTURE";
    public static final String SHADER_CUSTOM_PARAM_SCALE = "SCALE";
    public static final String SHADER_CUSTOM_PARAM_USE_INTERMEDIATE_FRAMEBUFFER = "USE_INTERMEDIATE_FRAMEBUFFER";
    public static final String SHADER_CUSTOM_PARAM_REPEAT_SOURCE_TEXTURE = "REPEAT_SOURCE_TEXTURE";
    public static final String SHADER_CUSTOM_PARAM_HISTORY_COUNT = "HISTORY_COUNT";

    public static final int ERROR_LOAD_LIBRARY = 0;
    public static final int ERROR_LOAD_GAME = 1;
    public static final int ERROR_GL_NOT_COMPATIBLE = 2;
    public static final int ERROR_SERIALIZATION = 3;
    public static final int ERROR_CHEAT = 4;
    public static final int ERROR_GENERIC = -1;

    public static final int RTC_TYPE_NO_OVERRIDE = 0;
    public static final int RTC_TYPE_FIXED = 1;
    public static final int RTC_TYPE_FAKE_EPOCH = 2;
    public static final int RTC_TYPE_WALLCLOCK_OFFSET = 3;
    public static final int RTC_TYPE_CUSTOM_START = 0x1000;

    public static final int HW_FLAG_NO_OVERRIDE = 0x8000;
    public static final int HW_FLAG_NONE = 0;
    public static final int HW_FLAG_RTC = 1;
    public static final int HW_FLAG_RUMBLE = 2;
    public static final int HW_FLAG_LIGHT_SENSOR = 4;
    public static final int HW_FLAG_GYRO = 8;
    public static final int HW_FLAG_TILT = 16;
    public static final int HW_FLAG_GB_PLAYER = 32;
    public static final int HW_FLAG_GB_PLAYER_DETECTION = 64;
    public static final int HW_FLAG_EREADER = 128;
    public static final int HW_FLAG_GPIO = HW_FLAG_RTC | HW_FLAG_RUMBLE | HW_FLAG_LIGHT_SENSOR | HW_FLAG_GYRO | HW_FLAG_TILT;

    public static final int SAVEDATA_TYPE_AUTODETECT = -1;
    public static final int SAVEDATA_TYPE_NONE = 0;
    public static final int SAVEDATA_TYPE_SRAM = 1;
    public static final int SAVEDATA_TYPE_FLASH512 = 2;
    public static final int SAVEDATA_TYPE_FLASH1M = 3;
    public static final int SAVEDATA_TYPE_EEPROM = 4;
    public static final int SAVEDATA_TYPE_EEPROM512 = 5;
    public static final int SAVEDATA_TYPE_SRAM512 = 6;

    public static final int LINK_EXCHANGE_TYPE_TRANSFER_START = 0;
    public static final int LINK_EXCHANGE_TYPE_MODE_CHANGED = 1;
    public static final int LINK_EXCHANGE_TYPE_FRAME_SYNC = 2;
    private static volatile LinkExchangeCallback sLinkExchangeCallback;

    public static native void create(
        int GLESVersion,
        String coreFilePath,
        String systemDir,
        String savesDir,
        Variable[] variables,
        GLRetroShader shaderConfig,
        float refreshRate,
        boolean preferLowLatencyAudio,
        boolean enableVirtualFileSystem,
        boolean enableMicrophone,
        boolean skipDuplicateFrames,
        ImmersiveMode immersiveMode,
        String language
    );

    public static native CoreInfo getCoreInfo();
    public static native void loadGameFromPath(String gameFilePath);
    public static native void loadGameFromBytes(byte[] gameFileBytes);
    static native void reloadGameFromBytes(byte[] gameFileBytes);
    static native void reloadGameFromBytesWithSystemDirectory(byte[] gameFileBytes, String systemDirectory);
    public static native void loadGameFromVirtualFiles(List<DetachedVirtualFile> virtualFiles);

    public static native void resume();

    public static native void onSurfaceCreated();
    public static native void onSurfaceChanged(int width, int height);

    public static native void pause();
    public static native boolean isRunning();
    public static native void destroy();

    public static native void step(GLRetroView retroView);
    static native void stepWithoutVideo(GLRetroView glRetroView);

    public static native void reset();

    public static native void setRumbleEnabled(boolean enabled);
    public static native void setFrameSpeed(float speed);
    public static native void setAudioEnabled(boolean enabled);
    public static native void setShaderConfig(GLRetroShader shader);
    public static native void setViewport(float x, float y, float width, float height);

    public static native byte[] serializeState();
    public static native boolean unserializeState(byte[] state);

    public static native byte[] getPixels(int[] outSize);

    /** An empty buffer queries the mapped bytes available at offset. */
    public static native int readMemoryData(int offset, byte[] buffer);

    // Optional mGBA extension: copy the loaded ROM with ROM-cheat patches undone
    // in the copy only. Null means that a safe rollback image is unavailable.
    static native byte[] copyUnpatchedRom();
    static native long getBiosIdentity();
    public static native void writeMemoryData(int offset, byte[] buffer);

    public static native void setCheat(int index, boolean enable, String code);
    public static native void resetCheat();

    public static native byte[] serializeSRAM();
    public static native boolean unserializeSRAM(byte[] sram);

    public static native void updateVariable(Variable variable);
    public static native Variable[] getVariables();

    public static native int availableDisks();
    public static native int currentDisk();
    public static native void changeDisk(int index);

    public static native void onMotionEvent(int port, int motionSource, float xAxis, float yAxis);
    public static native void onTouchEvent(float xAxis, float yAxis);

    public static native void onKeyEvent(int port, int action, int keyCode);

    public static native void refreshAspectRatio();
    public static native float getAspectRatio();
    public static native float getContentFrameRate();
    public static native void setAspectRatio(float aspectRatio);

    public static native Controller[][] getControllers();
    public static native void setControllerType(int port, int type);

    public static native void setAudioVolume(int volume); // 0~100，0为静音

    //void retro_set_rtc(unsigned type,int64_t value);
    public static native void setRTC(int type, long value);

    static native void setMultiplayer(int playerId, int playerCount);
    /** mode: 0 = legacy per-SIO transport, 1 = LAN shadow-core transport. */
    static native boolean configureLinkNetplay(int mode, int inputDelayFrames);
    static native int getLinkNetplayStatus();
    /** Unsigned CRC32 as a long, or -1 when no GBA ROM is loaded. */
    static native long getRomCrc32();
    /** Socket array indices are player IDs. Do not reuse descriptors after this call. */
    static native boolean setMultiplayerSockets(int[] socketFds, int playerId, int playerCount);
    static native boolean isNativeLinkActive();
    static native long linkRemoteStart(int sequence, int mode, int transferCycles, int primaryData);
    static native boolean linkRemoteMode(int playerId, int mode);
    static native boolean linkRemoteCommit(int sequence, int[] data);
    static native void linkCancel();
    static native boolean hasLinkSupport();

    static native boolean hasRomConsoleSupport();
    static native long getPrimaryRomConsoleHandle();
    static native long createRomConsoleHandle(byte[] romBytes, byte[] saveRam);
    static native boolean destroyRomConsoleHandle(long handle);
    static native long getRomConsoleCrc32(long handle);
    static native boolean startRomConsoleLanLink(int[] socketFds, long[] handles,
                                                  int localPlayerId);
    static native boolean startLocalLink(long[] handles);
    static native boolean setForegroundRomConsole(long handle);
    static native long getForegroundRomConsoleHandle();
    static native boolean resetRomConsole(long handle);
    static native byte[] getRomConsolePixels(long handle, int[] outSize);
    static native boolean resetRomConsoleCheat(long handle);
    static native boolean setRomConsoleCheat(long handle, int type, boolean enable, String code);
    static native byte[] serializeRomConsoleSRAM(long handle);
    static native byte[] serializeRomConsoleState(long handle);
    static native boolean unserializeRomConsoleState(long handle, byte[] state);
    static native boolean unserializeLanPlayerState(int playerId, byte[] state, int generation);
    static native boolean resetLanPlayer(int playerId, int generation);
    static native int pollLanStateBarrier(int generation);
    static native boolean unserializeLanResetState(int playerId, byte[] state, int generation);
    static native void stopLocalLink();

    static void setLinkExchangeCallback(LinkExchangeCallback callback) {
        sLinkExchangeCallback = callback;
    }

    static int linkExchangeCallback(int type, int[] args, short[] data) {
        LinkExchangeCallback callback = sLinkExchangeCallback;
        return callback != null ? callback.onLinkExchange(type, args, data) : 0;
    }

    static native void glJniEnvInit();

    static native void glJniEnvDestroy();


}
