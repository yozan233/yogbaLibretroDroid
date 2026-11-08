/*
 *     Copyright (C) 2022  Filippo Scognamiglio
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

package com.swordfish.libretrodroid

import android.app.ActivityManager
import android.app.Activity
import android.content.Context
import android.graphics.Bitmap
import android.graphics.PointF
import android.graphics.Rect
import android.graphics.RectF
import android.opengl.GLES20
import android.opengl.GLSurfaceView
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowManager
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.OnLifecycleEvent
import androidx.lifecycle.coroutineScope
import com.swordfish.libretrodroid.gamepad.GamepadsManager
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.nio.ByteBuffer
import java.util.*
import java.util.concurrent.ExecutionException
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10
import kotlin.properties.Delegates
import kotlin.coroutines.resume
import kotlin.coroutines.Continuation
import kotlin.coroutines.suspendCoroutine

class GLRetroView(
    context: Context,
    private val data: GLRetroViewData
) : GLSurfaceView(context), LifecycleObserver {

    @Deprecated("请使用 setAudioVolume(0) 或 setAudioVolume(100)")
    var audioEnabled: Boolean by Delegates.observable(true) { _, _, value ->
        if (ownsNativeSession()) LibretroDroid.setAudioEnabled(value)
    }

    var audioVolume: Int by Delegates.observable(100) { _, _, value ->
        if (ownsNativeSession()) LibretroDroid.setAudioVolume(value)
    }

    var frameSpeed: Float by Delegates.observable(1f) { _, _, value ->
        if (ownsNativeSession()) LibretroDroid.setFrameSpeed(value)
    }

    var shader: ShaderConfig by Delegates.observable(data.shader) { _, _, value ->
        if (ownsNativeSession()) LibretroDroid.setShaderConfig(buildShader(value))
    }

    var viewport: RectF by Delegates.observable(RectF(0f, 0f, 1f, 1f)) { _, _, value ->
        runOnGLThreadVoid {
            LibretroDroid.setViewport(value.left, value.top, value.width(), value.height())
        }
    }

    val isRunning: Boolean
        get() = ownsNativeSession() && isGameLoaded && LibretroDroid.isRunning()

    var rumbleEnabled: Boolean by Delegates.observable(true) { _, _, value ->
        if (ownsNativeSession()) LibretroDroid.setRumbleEnabled(value)
    }

    var aspectRatio: Float
        get() = if (ownsNativeSession() && isGameLoaded) LibretroDroid.getAspectRatio() else 1.5f
        set(value) { if (ownsNativeSession() && isGameLoaded) LibretroDroid.setAspectRatio(value) }

    val coreInfo: CoreInfo by lazy {
        check(ownsNativeSession()) { "Native session not ready" }
        LibretroDroid.getCoreInfo()
    }

    private val openGLESVersion: Int

    @Volatile
    private var isGameLoaded = false

    @Volatile
    private var isEmulationReady = false

    private var isAborted = false

    private val retroGLEventsSubject = MutableSharedFlow<GLRetroEvents>(extraBufferCapacity = 1)
    private val retroGLIssuesErrors = MutableSharedFlow<Int>(1)

    private val rumbleEventsSubject = MutableSharedFlow<RumbleEvent>()
    private val saveDataSyncedEventsSubject = MutableSharedFlow<Unit>(replay = 1)

    private var lifecycle: Lifecycle? = null

    private val glThread = AtomicReference<Thread?>()
    private val mainHandler = Handler(Looper.getMainLooper())
    private val captureAndPauseRequest = AtomicReference<CaptureAndPauseRequest?>(null)
    private val resumedFrameCallback = AtomicReference<(() -> Unit)?>(null)
    private var capturePixelBuffer: ByteBuffer? = null
    private val pausePreviewBitmaps = arrayOfNulls<Bitmap>(2)
    private var nextPausePreviewBitmap = 0
    private val romConsoles = mutableListOf<RomConsole>()

    @Volatile
    private var primaryRomConsole: RomConsole? = null

    @Volatile
    private var isDestroyed = false

    @Volatile
    private var nativeSessionReady = false
    private val nativeCleanupStarted = AtomicBoolean(false)
    @Volatile
    private var glThreadExited = false
    private var surfaceWidth = 0
    private var surfaceHeight = 0
    private val coreOperationLock = Any()
    private val coreOperations = mutableSetOf<CoreOperation<*>>()

    private fun ownsNativeSession() = !isDestroyed && nativeSessionReady && nativeSessionOwner.get() === this

    @Volatile
    private var isGLSurfacePaused = false

    var autoResume = true

    @Volatile
    private var keepRunningOnLifecyclePause = false

    private val backgroundStepping = AtomicBoolean(false)
    private val backgroundStepQueued = AtomicBoolean(false)
    private val backgroundStepRunnable = object : Runnable {
        override fun run() {
            backgroundStepQueued.set(false)
            if (!backgroundStepping.get() || isDestroyed || !isEmulationReady || isAborted)
                return
            catchExceptions {
                // This task still runs on GLSurfaceView's GL thread, but does not
                // require an EGL surface. Keeping the same thread preserves the
                // single-threaded retro_run() contract while the Activity is stopped.
                LibretroDroid.stepWithoutVideo(this@GLRetroView)
            }
            scheduleBackgroundStep()
        }
    }

    init {
        openGLESVersion = getGLESVersion(context)
        preserveEGLContextOnPause = true
        setEGLContextClientVersion(openGLESVersion)
        setRenderer(Renderer())
        keepScreenOn = true
    }

    @OnLifecycleEvent(Lifecycle.Event.ON_CREATE)
    fun onCreate(lifecycleOwner: LifecycleOwner) = catchExceptions {
        lifecycle = lifecycleOwner.lifecycle
        val variables = data.variables
        variables.add(Variable("mgba_gba_hardware", data.hardware.toString()))
        variables.add(Variable("mgba_gba_savetype", data.savetype.toString()))

        val createNow = synchronized(nativeSessionLock) {
            if (nativeSessionOwner.compareAndSet(null, this)) true
            else {
                if (!pendingNativeSessions.contains(this)) pendingNativeSessions.add(this)
                false
            }
        }
        if (createNow) createNativeSession()
    }

    private fun createNativeSession() = catchExceptions {
        if (isDestroyed || nativeSessionOwner.get() !== this) return@catchExceptions
        LibretroDroid.create(
            openGLESVersion,
            data.coreFilePath,
            data.systemDirectory,
            data.savesDirectory,
            data.variables.toTypedArray(),
            buildShader(data.shader),
            getDefaultRefreshRate(),
            data.preferLowLatencyAudio,
            data.gameVirtualFiles.isNotEmpty(),
            data.enableMicrophone,
            data.skipDuplicateFrames,
            data.immersiveMode,
            getDeviceLanguage()
        )
        nativeSessionReady = true
        LibretroDroid.setAudioVolume(audioVolume)
        LibretroDroid.setFrameSpeed(frameSpeed)
        LibretroDroid.setShaderConfig(buildShader(shader))
        rumbleEnabled = data.rumbleEventsEnabled
        //LibretroDroid.setLinkExchangeCallback(this)

        runOnGLThreadVoid {
            LibretroDroid.glJniEnvInit()
        }
        // A queued event is allowed to run without any EGL surface. Initialize
        // the renderer only from a real onSurfaceCreated/onDrawFrame callback.
        requestRender()

    }

    @OnLifecycleEvent(Lifecycle.Event.ON_DESTROY)
    fun onDestroy() {
        if (isDestroyed) return
        isDestroyed = true
        synchronized(coreOperationLock) { coreOperations.toList() }.forEach { it.abort() }
        takeCaptureAndPauseRequest()
        resumedFrameCallback.set(null)
        stopBackgroundStepping(false)
        isEmulationReady = false
        renderMode = RENDERMODE_WHEN_DIRTY
        synchronized(nativeSessionLock) { pendingNativeSessions.remove(this) }
        if (nativeSessionOwner.get() === this) {
            // Mandatory cleanup must never be cancelled by a query timeout.
            // Ownership is retained until it finishes; a new create waits.
            if (glThreadExited) startDetachedCleanup()
            else if (Thread.currentThread() === glThread.get()) destroyNativeSession()
            else queueEvent { destroyNativeSession() }
        } else {
            clearSessionReferences()
        }
        lifecycle = null
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        glThreadExited = false
    }

    override fun onDetachedFromWindow() {
        // A temporary reparent/detach is not an Activity destruction.
        if (lifecycle?.currentState == Lifecycle.State.DESTROYED ||
            (context as? Activity)?.isFinishing == true) onDestroy()
        // GLSurfaceView joins its GL thread here. Exit can take priority over a
        // queued event, so do not assume that queued cleanup was consumed.
        super.onDetachedFromWindow()
        glThreadExited = true
        synchronized(coreOperationLock) { coreOperations.toList() }.forEach { it.abort() }
        if (isDestroyed) startDetachedCleanup()
    }

    private fun startDetachedCleanup() {
        if (nativeSessionOwner.get() === this && !nativeCleanupStarted.get())
            Thread({ destroyNativeSession() }, "RetroSessionCleanup").start()
    }

    private fun destroyNativeSession() {
        if (nativeSessionOwner.get() !== this || !nativeCleanupStarted.compareAndSet(false, true)) return
        try {
            // Normally on the owning GL thread. The detach fallback runs only
            // after that thread has exited, with no renderer/retro_run left.
            LibretroDroid.glJniEnvDestroy()
            LibretroDroid.destroy()
            nativeSessionReady = false
            clearSessionReferences()
            mainHandler.post {
                val next = synchronized(nativeSessionLock) {
                    nativeSessionOwner.compareAndSet(this, null)
                    pendingNativeSessions.removeAll { it.isDestroyed }
                    pendingNativeSessions.firstOrNull()?.also {
                        pendingNativeSessions.removeAt(0)
                        nativeSessionOwner.set(it)
                    }
                }
                next?.createNativeSession()
            }
        } catch (e: Throwable) {
            // Do not hand a partially destroyed singleton to another Activity.
            Log.e(TAG_LOG, "Native session cleanup failed", e)
        }
    }

    private fun clearSessionReferences() {
        data.gameFileBytes = null
        data.saveRAMState = null
        romConsoles.clear()
        primaryRomConsole = null
        capturePixelBuffer = null
        pausePreviewBitmaps.fill(null)
        glThread.set(null)
    }

//    override fun onLinkExchange(type: Int, args: IntArray, data: ShortArray): Int {
//        return linkExchangeCallback?.onLinkExchange(type, args, data) ?: 0
//    }

    fun resume() = resume(null)

    fun resume(onFirstFrame: (() -> Unit)?){
        if (!ownsNativeSession() || !isGameLoaded) return
        resumedFrameCallback.set(onFirstFrame)
        // Activity/dialog focus changes may request resume even though WLAN
        // lockstep never paused. Native resume recreates Input and resets frame
        // pacing, so calling it concurrently with retro_run can strand one peer
        // while the others wait for its next input frame.
        if (!LibretroDroid.isRunning()) {
            LibretroDroid.resume()
        }
        renderMode = RENDERMODE_CONTINUOUSLY
        isEmulationReady = true
        if (isGLSurfacePaused) {
            onResume()
            isGLSurfacePaused = false
        }
        if (keepRunningOnLifecyclePause &&
            lifecycle?.currentState?.isAtLeast(Lifecycle.State.STARTED) == false) {
            startBackgroundStepping()
        }
        else {
            stopBackgroundStepping(true)
        }
    }

    fun runOnNextFrame(callback: () -> Unit){
        if (isDestroyed) return
        resumedFrameCallback.set(callback)
        requestRender()
    }

    /** For application tasks that must not outlive this native session. */
    fun queueEmulationEvent(action: () -> Unit) = runOnGLThreadVoid(action)

    fun pause(){
        pauseInternal(true)
    }

    /**
     * Stops emulation without pausing the GLSurfaceView thread. This is used
     * while an external picker covers the Activity: queued console setup work
     * must remain attached to the current GL thread when the picker returns.
     */
    fun pauseEmulationOnly(){
        pauseInternal(false)
    }

    private fun pauseInternal(pauseSurface: Boolean){
        if (!ownsNativeSession() || !isGameLoaded) return
        val captureRequest = takeCaptureAndPauseRequest()
        stopBackgroundStepping(false)
        isEmulationReady = false
        renderMode = RENDERMODE_WHEN_DIRTY
        if (pauseSurface && !isGLSurfacePaused) {
            onPause()
            isGLSurfacePaused = true
        }
        LibretroDroid.pause()
        captureRequest?.let {
            KtUtils.runOnUIThread { it.block(null) }
        }
    }

    fun captureAndPause(captureRect: Rect?, block: (Bitmap?) -> Unit, needPause: Boolean) {
        if (isDestroyed || !isEmulationReady) {
            KtUtils.runOnUIThread { block(null) }
            return
        }
        val request = CaptureAndPauseRequest(
            captureRect?.let { Rect(it) },
            block,
            needPause
        )
        val timeoutRunnable = Runnable {
            if (captureAndPauseRequest.compareAndSet(request, null)) {
                // GLSurfaceView.onPause() waits for the GL thread. A local-link
                // frame can legitimately exceed this short capture timeout, so
                // calling the full pause here on the UI thread can cause an ANR.
                // Stop scheduling frames immediately and serialize the native
                // pause behind the current GL task without blocking input.
                if (request.needPause) {
                    isEmulationReady = false
                    renderMode = RENDERMODE_WHEN_DIRTY
                    queueEvent {
                        // resume() may run before this queued task if the user closes
                        // the dialog quickly. Do not let a stale timeout pause a
                        // session that has already resumed.
                        if (!isDestroyed && !isEmulationReady) {
                            LibretroDroid.pause()
                        }
                    }
                }
                block(null)
            }
        }
        request.timeoutRunnable = timeoutRunnable

        if (!captureAndPauseRequest.compareAndSet(null, request)) {
            KtUtils.runOnUIThread { block(null) }
            return
        }

        mainHandler.postDelayed(timeoutRunnable, CAPTURE_AND_PAUSE_TIMEOUT_MS)
        // The GL thread can claim the request between compareAndSet() and postDelayed().
        if (captureAndPauseRequest.get() !== request)
            mainHandler.removeCallbacks(timeoutRunnable)
        requestRender()
    }

    private fun getDeviceLanguage() = Locale.getDefault().language

    private fun getDefaultRefreshRate(): Float {
        return (context.getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.refreshRate
    }

    fun sendKeyEvent(action: Int, keyCode: Int, port: Int = 0) {
        if (ownsNativeSession())
            LibretroDroid.onKeyEvent(port, action, keyCode)
    }

    fun sendMotionEvent(source: Int, xAxis: Float, yAxis: Float, port: Int = 0) {
        runOnGLThreadVoid { LibretroDroid.onMotionEvent(port, source, xAxis, yAxis) }
    }

    override fun onTouchEvent(event: MotionEvent?): Boolean {
        val position = when (event?.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                normalizeTouchCoordinates(event.x, event.y)
            }
            MotionEvent.ACTION_UP -> {
                TOUCH_EVENT_OUTSIDE
            }
            else -> null
        }

        if (position != null && ownsNativeSession()) {
            LibretroDroid.onTouchEvent(position.x, position.y)
        }

        return true
    }

    private fun clamp(x: Float, min: Float, max: Float) = minOf(maxOf(x, min), max)

    private fun normalizeTouchCoordinates(x: Float, y: Float): PointF {
        val x = clamp(2f * x / width - 1f, -1f, +1f)
        val y = clamp(2f * y / height - 1f, -1f, +1f)
        return PointF(x, y)
    }

    fun serializeState(): ByteArray? = runOnGLThreadOrNull {
        if (!isGameLoaded || isAborted) return@runOnGLThreadOrNull null
        LibretroDroid.serializeState()
    }
    fun setCheat(type : Int, enable : Boolean, code : String) = runOnGLThreadVoid {
        LibretroDroid.setCheat(type, enable, code)
    }
    fun resetCheat() = runOnGLThreadVoid {
        LibretroDroid.resetCheat()
    }

    fun unserializeState(data: ByteArray): Boolean = runOnGLThread(false) {
        LibretroDroid.unserializeState(data)
    }

    fun serializeSRAM(): ByteArray = runOnGLThread(ByteArray(0)) {
        if (!isGameLoaded || isAborted) ByteArray(0) else LibretroDroid.serializeSRAM()
    }

    fun unserializeSRAM(data: ByteArray): Boolean = runOnGLThread(false) {
        coreUnserializeSRAM(data)
    }

    fun reset() = runOnGLThreadVoid {
        LibretroDroid.reset()
    }

    fun screenshot(): ScreenshotInfo = runOnGLThread(ScreenshotInfo()) {
        val outSize = IntArray(5)
        val bytes = LibretroDroid.getPixels(outSize)
        if (bytes == null || bytes.isEmpty()){
            ScreenshotInfo()
        }
        else{
            ScreenshotInfo(
                bytes,
                outSize[0],
                outSize[1],
                outSize[4],
                outSize[3]
            )
        }
    }

    fun setRTC(type: Int, value: Long) {
        if (ownsNativeSession() && isGameLoaded) LibretroDroid.setRTC(type, value)
    }

    fun getGLRetroEvents(): Flow<GLRetroEvents> {
        return retroGLEventsSubject
    }

    fun getGLRetroErrors(): Flow<Int> {
        return retroGLIssuesErrors
    }

    fun getRumbleEvents(): Flow<RumbleEvent> {
        return rumbleEventsSubject
    }

    fun getSaveDataSyncedEvents(): Flow<Unit> {
        return saveDataSyncedEventsSubject
    }

    fun getControllers(): Array<Array<Controller>> {
        return if (ownsNativeSession()) LibretroDroid.getControllers() else emptyArray()
    }

    fun setControllerType(port: Int, type: Int) {
        if (ownsNativeSession()) LibretroDroid.setControllerType(port, type)
    }

    fun getVariables(): Array<Variable> {
        return if (ownsNativeSession()) LibretroDroid.getVariables() else data.variables.toTypedArray()
    }

    fun updateVariables(vararg variables: Variable) {
        if (!ownsNativeSession()) return
        variables.forEach {
            LibretroDroid.updateVariable(it)
        }
    }

    fun getAvailableDisks():Int = runOnGLThread(0) { LibretroDroid.availableDisks() }
    fun getCurrentDisk():Int = runOnGLThread(0) { LibretroDroid.currentDisk() }
    fun changeDisk(index: Int) = runOnGLThreadVoid { LibretroDroid.changeDisk(index) }

    fun getPrimaryRomConsole(): RomConsole? = primaryRomConsole

    fun createRomConsole(romBytes: ByteArray, saveRAMState: ByteArray? = null): RomConsole? =
        runOnGLThreadOrNull {
            if (!LibretroDroid.hasRomConsoleSupport()) return@runOnGLThreadOrNull null
            val playerId = romConsoles.size
            if (playerId !in 1..3) return@runOnGLThreadOrNull null
            val handle = LibretroDroid.createRomConsoleHandle(romBytes, saveRAMState)
            if (handle == 0L) return@runOnGLThreadOrNull null
            val crc32 = LibretroDroid.getRomConsoleCrc32(handle)
            if (crc32 < 0L) return@runOnGLThreadOrNull null
            RomConsole(this, handle, playerId, crc32).also { romConsoles.add(it) }
        }

    fun destroyRomConsole(console: RomConsole): Boolean = runOnGLThread(false) {
        val playerId = console.playerId
        if (playerId <= 0 || playerId != romConsoles.lastIndex ||
            romConsoles.getOrNull(playerId) !== console) {
            return@runOnGLThread false
        }
        if (!LibretroDroid.destroyRomConsoleHandle(console.nativeHandle)) {
            return@runOnGLThread false
        }
        romConsoles.removeAt(playerId)
        true
    }

    fun startLocalLink(vararg consoles: RomConsole): Boolean = runOnGLThread(false) {
        if (consoles.size !in 2..4) return@runOnGLThread false
        val ordered = consoles.sortedBy { it.playerId }
        if (ordered.indices.any { index ->
                ordered[index].playerId != index || romConsoles.getOrNull(index) !== ordered[index]
            }) {
            return@runOnGLThread false
        }
        LibretroDroid.startLocalLink(ordered.map { it.nativeHandle }.toLongArray())
    }

    fun startLocalLinkWithForeground(foreground: RomConsole, vararg consoles: RomConsole): Boolean = runOnGLThread(false) {
        if (consoles.size !in 2..4) return@runOnGLThread false
        val ordered = consoles.sortedBy { it.playerId }
        if (ordered.indices.any { index ->
                ordered[index].playerId != index ||
                        romConsoles.getOrNull(index) !== ordered[index]
            } || foreground !in ordered) {
            return@runOnGLThread false
        }
        if (!LibretroDroid.startLocalLink(
                ordered.map { it.nativeHandle }.toLongArray()
            )) {
            return@runOnGLThread false
        }
        if (!LibretroDroid.setForegroundRomConsole(foreground.nativeHandle)) {
            LibretroDroid.stopLocalLink()
            return@runOnGLThread false
        }
        true
    }

    fun startLanLink(socketFds: IntArray, localPlayerId: Int, vararg players: RomConsole): Boolean = runOnGLThread(false) {
        if (socketFds.size < 4 || players.size !in 2..4 ||
            localPlayerId !in players.indices || players[localPlayerId] !== primaryRomConsole ||
            players.size != romConsoles.size) {
            return@runOnGLThread false
        }
        if (players.toSet().size != players.size || players.any { console ->
                romConsoles.getOrNull(console.playerId) !== console
            }) {
            return@runOnGLThread false
        }
        if (!LibretroDroid.configureLinkNetplay(1, DEFAULT_LINK_INPUT_DELAY_FRAMES)) {
            return@runOnGLThread false
        }
        LibretroDroid.startRomConsoleLanLink(
            socketFds,
            players.map { it.nativeHandle }.toLongArray(),
            localPlayerId
        )
    }

    /**
     * Creates the application-selected remote consoles and starts LAN lockstep
     * in one GL task. The array is indexed by link player ID; the local entry is
     * ignored and every remote entry must contain that player's ROM bytes.
     *
     * Socket ownership transfers to native only when [onComplete] receives a
     * non-null list. On failure the caller still owns every descriptor.
     */
    fun startLanLink(
        socketFds: IntArray,
        localPlayerId: Int,
        playerRomBytes: Array<ByteArray?>,
        inputDelayFrames: Int = DEFAULT_LINK_INPUT_DELAY_FRAMES,
        onComplete: (List<RomConsole>?) -> Unit
    ) {
        if (isDestroyed) {
            KtUtils.runOnUIThread { onComplete(null) }
            return
        }
        val descriptors = socketFds.copyOf()
        val romBytes = playerRomBytes.copyOf()
        queueEvent {
            val result = try {
                if (!ownsNativeSession()) null else startLanLinkOnGLThread(
                    descriptors,
                    localPlayerId,
                    romBytes,
                    inputDelayFrames
                )
            }
            catch (e:Exception) {
                /* LINK_DIAGNOSTIC_DISABLED: Log.e(TAG_LOG, "Failed to create LAN ROM consoles", e) */
                null
            }
            KtUtils.runOnUIThread { onComplete(result) }
        }
    }

    private fun startLanLinkOnGLThread(
        socketFds: IntArray,
        localPlayerId: Int,
        playerRomBytes: Array<ByteArray?>,
        inputDelayFrames: Int
    ): List<RomConsole>? {
        val playerCount = playerRomBytes.size
        val primary = primaryRomConsole
        if (!LibretroDroid.hasRomConsoleSupport() || socketFds.size < 4 ||
            playerCount !in 2..4 ||
            localPlayerId !in playerRomBytes.indices || primary == null ||
            inputDelayFrames !in MIN_LINK_INPUT_DELAY_FRAMES..MAX_LINK_INPUT_DELAY_FRAMES ||
            romConsoles.size != 1 || romConsoles.firstOrNull() !== primary) {
            return null
        }

        if (!LibretroDroid.configureLinkNetplay(1, inputDelayFrames)) {
            return null
        }

        val players = arrayOfNulls<RomConsole>(playerCount)
        val created = arrayListOf<RomConsole>()
        players[localPlayerId] = primary

        fun rollback(): List<RomConsole>? {
            created.asReversed().forEach { console ->
                if (romConsoles.lastOrNull() === console &&
                    LibretroDroid.destroyRomConsoleHandle(console.nativeHandle)) {
                    romConsoles.removeAt(romConsoles.lastIndex)
                }
            }
            return null
        }

        for (linkPlayerId in players.indices) {
            if (linkPlayerId == localPlayerId)
                continue
            val bytes = playerRomBytes[linkPlayerId]
                ?.takeIf { it.isNotEmpty() } ?: return rollback()
            val handle = LibretroDroid.createRomConsoleHandle(bytes, null)
            if (handle == 0L)
                return rollback()
            val crc32 = LibretroDroid.getRomConsoleCrc32(handle)
            if (crc32 < 0L) {
                LibretroDroid.destroyRomConsoleHandle(handle)
                return rollback()
            }
            val console = RomConsole(this, handle, romConsoles.size, crc32)
            romConsoles.add(console)
            created.add(console)
            players[linkPlayerId] = console
        }

        val handles = LongArray(playerCount)
        for (playerId in players.indices) {
            handles[playerId] = players[playerId]?.nativeHandle ?: return rollback()
        }
        if (!LibretroDroid.startRomConsoleLanLink(socketFds, handles, localPlayerId)) {
            return rollback()
        }
        return created
    }

    /** Applies the room-wide speed and its delayed-input depth on the GL thread. */
    fun setLanLinkFrameSpeed(
        speed: Float,
        inputDelayFrames: Int,
        onComplete: (Boolean) -> Unit = {}
    ) {
        if (isDestroyed || !speed.isFinite() || speed < 0.2f || speed > 3f ||
            inputDelayFrames !in MIN_LINK_INPUT_DELAY_FRAMES..MAX_LINK_INPUT_DELAY_FRAMES) {
            KtUtils.runOnUIThread { onComplete(false) }
            return
        }
        queueEvent {
            val success = try {
                if (ownsNativeSession() && LibretroDroid.configureLinkNetplay(1,inputDelayFrames)) {
                    frameSpeed = speed
                    true
                }
                else {
                    false
                }
            }
            catch (e:Exception) {
                /* LINK_DIAGNOSTIC_DISABLED: Log.e(TAG_LOG,"Failed to apply LAN frame speed",e) */
                false
            }
            KtUtils.runOnUIThread { onComplete(success) }
        }
    }

    /** Queues a normal-speed reset after every previously queued WLAN speed change. */
    fun resetLanLinkFrameSpeed(onComplete: (Boolean) -> Unit = {}) {
        if (isDestroyed) {
            KtUtils.runOnUIThread { onComplete(false) }
            return
        }
        queueEvent {
            val success = ownsNativeSession()
            if (success) frameSpeed = 1f
            KtUtils.runOnUIThread { onComplete(success) }
        }
    }

    /** Stops LAN and removes exactly the remote consoles created for that session. */
    fun stopLanLinkAndDestroyConsoles(
        consoles: Collection<RomConsole>,
        onComplete: (Boolean) -> Unit = {}
    ) {
        if (isDestroyed) {
            KtUtils.runOnUIThread { onComplete(false) }
            return
        }
        val targets = consoles.toSet()
        queueEvent {
            var success = true
            try {
                check(ownsNativeSession())
                LibretroDroid.stopLocalLink()
                targets.sortedByDescending { it.playerId }.forEach { console ->
                    if (romConsoles.lastOrNull() !== console ||
                        !LibretroDroid.destroyRomConsoleHandle(console.nativeHandle)) {
                        success = false
                        return@forEach
                    }
                    romConsoles.removeAt(romConsoles.lastIndex)
                }
            }
            catch (e:Exception) {
                /* LINK_DIAGNOSTIC_DISABLED: Log.e(TAG_LOG, "Failed to destroy LAN ROM consoles", e) */
                success = false
            }
            KtUtils.runOnUIThread { onComplete(success) }
        }
    }

    /** Keep WLAN emulation stepping when the Activity is paused or fully stopped. */
    fun setKeepRunningOnLifecyclePause(enabled: Boolean) {
        keepRunningOnLifecyclePause = enabled
        if (enabled) {
            if (lifecycle?.currentState?.isAtLeast(Lifecycle.State.STARTED) == false) {
                startBackgroundStepping()
            }
        }
        else {
            stopBackgroundStepping(true)
            if (!isDestroyed &&
                lifecycle?.currentState?.isAtLeast(Lifecycle.State.RESUMED) != true) {
                pause()
            }
        }
    }

    private fun startBackgroundStepping() {
        if (!keepRunningOnLifecyclePause || isDestroyed || !isEmulationReady || isAborted)
            return
        if (backgroundStepping.compareAndSet(false, true)) {
            /* LINK_DIAGNOSTIC_DISABLED: Log.i(TAG_LOG, "Background emulation stepping started") */
        }
        renderMode = RENDERMODE_WHEN_DIRTY
        scheduleBackgroundStep()
    }

    private fun stopBackgroundStepping(resumeRendering: Boolean) {
        if (backgroundStepping.getAndSet(false)) {
            /* LINK_DIAGNOSTIC_DISABLED: Log.i(TAG_LOG, "Background emulation stepping stopped") */
        }
        if (resumeRendering && !isDestroyed && isEmulationReady) {
            renderMode = RENDERMODE_CONTINUOUSLY
            requestRender()
        }
    }

    private fun scheduleBackgroundStep() {
        if (!backgroundStepping.get() || isDestroyed || !isEmulationReady || isAborted)
            return
        if (!backgroundStepQueued.compareAndSet(false, true))
            return
        try {
            queueEvent(backgroundStepRunnable)
        }
        catch (e: RuntimeException) {
            backgroundStepQueued.set(false)
            /* LINK_DIAGNOSTIC_DISABLED: Log.e(TAG_LOG, "Failed to queue background emulation step", e) */
        }
    }

    fun setForegroundRomConsole(console: RomConsole): Boolean = runOnGLThread(false) {
        if (romConsoles.getOrNull(console.playerId) !== console) return@runOnGLThread false
        LibretroDroid.setForegroundRomConsole(console.nativeHandle)
    }

    fun isForegroundRomConsole(console: RomConsole): Boolean = runOnGLThread(false) {
        romConsoles.getOrNull(console.playerId) === console &&
                LibretroDroid.getForegroundRomConsoleHandle() == console.nativeHandle
    }

    fun resetRomConsole(console: RomConsole): Boolean = runOnGLThread(false) {
        romConsoles.getOrNull(console.playerId) === console &&
                LibretroDroid.resetRomConsole(console.nativeHandle)
    }

    fun screenshotRomConsole(console: RomConsole): ScreenshotInfo =
        runOnGLThread(ScreenshotInfo()) {
            if (romConsoles.getOrNull(console.playerId) !== console) {
                return@runOnGLThread ScreenshotInfo()
            }
            val outSize = IntArray(5)
            val bytes = LibretroDroid.getRomConsolePixels(console.nativeHandle, outSize)
            if (bytes == null || bytes.isEmpty()) {
                ScreenshotInfo()
            } else {
                ScreenshotInfo(
                    bytes,
                    outSize[0],
                    outSize[1],
                    outSize[4],
                    outSize[3]
                )
            }
        }

    fun resetRomConsoleCheat(console: RomConsole): Boolean = runOnGLThread(false) {
        romConsoles.getOrNull(console.playerId) === console &&
                LibretroDroid.resetRomConsoleCheat(console.nativeHandle)
    }

    fun setRomConsoleCheat(
        console: RomConsole,
        type: Int,
        enable: Boolean,
        code: String
    ): Boolean = runOnGLThread(false) {
        romConsoles.getOrNull(console.playerId) === console && code.isNotEmpty() &&
                LibretroDroid.setRomConsoleCheat(console.nativeHandle, type, enable, code)
    }

    fun serializeRomConsoleSRAM(console: RomConsole): ByteArray? = runOnGLThreadOrNull {
        if (!isGameLoaded || isAborted) return@runOnGLThreadOrNull null
        if (romConsoles.getOrNull(console.playerId) !== console) return@runOnGLThreadOrNull null
        LibretroDroid.serializeRomConsoleSRAM(console.nativeHandle)
    }

    fun serializeRomConsoleState(console: RomConsole): ByteArray? = runOnGLThreadOrNull {
        if (!isGameLoaded || isAborted) return@runOnGLThreadOrNull null
        if (romConsoles.getOrNull(console.playerId) !== console) return@runOnGLThreadOrNull null
        LibretroDroid.serializeRomConsoleState(console.nativeHandle)
    }

    fun unserializeRomConsoleState(console: RomConsole, state: ByteArray): Boolean =
        runOnGLThread(false) {
            if (state.isEmpty() || romConsoles.getOrNull(console.playerId) !== console) {
                return@runOnGLThread false
            }
            val wasLinkActive = LibretroDroid.getLinkNetplayStatus() ==
                    RomConsole.LINK_STATUS_ACTIVE
            val foregroundHandle = LibretroDroid.getForegroundRomConsoleHandle()
            if (wasLinkActive) {
                LibretroDroid.stopLocalLink()
            }
            val loaded = LibretroDroid.unserializeRomConsoleState(
                console.nativeHandle,
                state
            )
            var restarted = true
            if (wasLinkActive) {
                restarted = LibretroDroid.startLocalLink(
                    romConsoles.map { it.nativeHandle }.toLongArray()
                )
                if (restarted && foregroundHandle != 0L) {
                    LibretroDroid.setForegroundRomConsole(foregroundHandle)
                }
            }
            loaded && restarted
        }

    /** Applies one room player's state while the WLAN control-plane barrier is held. */
    fun unserializeLanPlayerState(
        playerId: Int,
        state: ByteArray,
        generation: Int
    ): Boolean = runOnGLThread(false) {
        state.isNotEmpty() && playerId in 0 until romConsoles.size && generation > 0 &&
                LibretroDroid.unserializeLanPlayerState(playerId, state, generation)
    }

    /** Resets one room player's corresponding core while the WLAN barrier is held. */
    fun resetLanPlayer(playerId: Int, generation: Int): Boolean = runOnGLThread(false) {
        playerId in 0 until romConsoles.size && generation > 0 &&
                LibretroDroid.resetLanPlayer(playerId, generation)
    }

    /** Call only while ordinary stepping is paused; -1 failed, 0 pending, 1 aligned. */
    fun pollLanStateBarrier(generation: Int): Int = runOnGLThread(-1) {
        LibretroDroid.pollLanStateBarrier(generation)
    }

    fun unserializeLanResetState(
        playerId: Int, state: ByteArray, generation: Int
    ): Boolean = runOnGLThread(false) {
        state.isNotEmpty() && playerId in 0 until romConsoles.size && generation > 0 &&
                LibretroDroid.unserializeLanResetState(playerId, state, generation)
    }

    fun getRomConsoleLinkStatus(): Int = runOnGLThread(0) {
        LibretroDroid.getLinkNetplayStatus()
    }

    /** Reads link status without blocking the caller (normally the UI thread). */
    fun getRomConsoleLinkStatus(onComplete: (Int) -> Unit) {
        if (isDestroyed) {
            KtUtils.runOnUIThread { onComplete(RomConsole.LINK_STATUS_OFF) }
            return
        }
        queueEvent {
            val status = if (!ownsNativeSession()) RomConsole.LINK_STATUS_OFF
            else LibretroDroid.getLinkNetplayStatus()
            KtUtils.runOnUIThread { onComplete(status) }
        }
    }

    fun isRomConsoleLinkActive(): Boolean = runOnGLThread(false) {
        LibretroDroid.getLinkNetplayStatus() == RomConsole.LINK_STATUS_ACTIVE
    }

    fun stopLocalLink() = runOnGLThreadVoid {
        LibretroDroid.stopLocalLink()
    }

    private fun getGLESVersion(context: Context): Int {
        val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        return if (activityManager.deviceConfigurationInfo.reqGlEsVersion >= 0x30000) { 3 } else { 2 }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        val mappedKey = GamepadsManager.getGamepadKeyEvent(keyCode)
        val port = (event?.device?.controllerNumber ?: 0) - 1

        if (event != null && port >= 0 && keyCode in GamepadsManager.GAMEPAD_KEYS) {
            sendKeyEvent(KeyEvent.ACTION_DOWN, mappedKey, port)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent?): Boolean {
        val mappedKey = GamepadsManager.getGamepadKeyEvent(keyCode)
        val port = (event?.device?.controllerNumber ?: 0) - 1

        if (event != null && port >= 0 && keyCode in GamepadsManager.GAMEPAD_KEYS) {
            sendKeyEvent(KeyEvent.ACTION_UP, mappedKey, port)
            return true
        }
        return super.onKeyUp(keyCode, event)
    }

    override fun onGenericMotionEvent(event: MotionEvent?): Boolean {
        val port = (event?.device?.controllerNumber ?: 0) - 1
        if (port >= 0) {
            when (event?.source) {
                InputDevice.SOURCE_JOYSTICK -> {
                    sendMotionEvent(
                        MOTION_SOURCE_DPAD,
                        event.getAxisValue(MotionEvent.AXIS_HAT_X),
                        event.getAxisValue(MotionEvent.AXIS_HAT_Y),
                        port
                    )
                    sendMotionEvent(
                        MOTION_SOURCE_ANALOG_LEFT,
                        event.getAxisValue(MotionEvent.AXIS_X),
                        event.getAxisValue(MotionEvent.AXIS_Y),
                        port
                    )
                    sendMotionEvent(
                        MOTION_SOURCE_ANALOG_RIGHT,
                        event.getAxisValue(MotionEvent.AXIS_Z),
                        event.getAxisValue(MotionEvent.AXIS_RZ),
                        port
                    )
                }
            }
        }
        return super.onGenericMotionEvent(event)
    }

    // These functions are called only after the GLSurfaceView has been created.
    private inner class RenderLifecycleObserver : LifecycleObserver {
        @OnLifecycleEvent(Lifecycle.Event.ON_START)
        private fun lifecycleStart() = catchExceptions {
            stopBackgroundStepping(true)
        }

        @OnLifecycleEvent(Lifecycle.Event.ON_RESUME)
        private fun lifecycleResume() = catchExceptions {
            if (autoResume)
                resume()
        }

        @OnLifecycleEvent(Lifecycle.Event.ON_PAUSE)
        private fun lifecyclePause() = catchExceptions {
            if (!keepRunningOnLifecyclePause)
                pause()
        }

        @OnLifecycleEvent(Lifecycle.Event.ON_STOP)
        private fun lifecycleStop() = catchExceptions {
            if (keepRunningOnLifecyclePause)
                startBackgroundStepping()
        }
    }

    inner class Renderer : GLSurfaceView.Renderer {
        override fun onDrawFrame(gl: GL10) = catchExceptions {
            if (!ownsNativeSession()) return@catchExceptions
            // onSurfaceCreated may have happened while the preceding native
            // session was still being destroyed. This callback has a real EGL
            // surface, unlike queueEvent, and is safe for delayed initialization.
            if (!isGameLoaded) {
                initializeCore()
                if (!isGameLoaded) return@catchExceptions
                if (surfaceWidth > 0 && surfaceHeight > 0)
                    LibretroDroid.onSurfaceChanged(surfaceWidth, surfaceHeight)
                lifecycle?.coroutineScope?.launch {
                    retroGLEventsSubject.emit(GLRetroEvents.SurfaceCreated)
                }
            }
            if (ownsNativeSession() && isEmulationReady && !backgroundStepping.get()) {
                LibretroDroid.step(this@GLRetroView)

                resumedFrameCallback.getAndSet(null)?.let { callback ->
                    KtUtils.runOnUIThread { if (ownsNativeSession()) callback() }
                }

                takeCaptureAndPauseRequest()?.let { request ->
                    val bitmap = try {
                        captureRenderedFrame(gl, request.captureRect)
                    } catch (e: Exception) {
                        Log.w(TAG_LOG, "Failed to capture the rendered frame", e)
                        null
                    }
                    if (request.needPause) {
                        isEmulationReady = false
                        renderMode = RENDERMODE_WHEN_DIRTY
                        LibretroDroid.pause()
                    }
                    KtUtils.runOnUIThread {
                        request.block(bitmap)
                    }
                }

                lifecycle?.coroutineScope?.launch {
                    retroGLEventsSubject.emit(GLRetroEvents.FrameRendered)
                }
            }
        }

        override fun onSurfaceChanged(gl: GL10, width: Int, height: Int) = catchExceptions {
            surfaceWidth = width
            surfaceHeight = height
            if (!ownsNativeSession() || !isGameLoaded) return@catchExceptions
            Thread.currentThread().priority = Thread.MAX_PRIORITY
            LibretroDroid.onSurfaceChanged(width, height)
        }


        override fun onSurfaceCreated(gl: GL10, config: EGLConfig) = catchExceptions {
            glThread.set(Thread.currentThread())
            if (!ownsNativeSession()) return@catchExceptions

            Thread.currentThread().priority = Thread.MAX_PRIORITY
            if (isGameLoaded) LibretroDroid.onSurfaceCreated() else initializeCore()
            lifecycle?.coroutineScope?.launch {
                retroGLEventsSubject.emit(GLRetroEvents.SurfaceCreated)
            }
        }
    }

    private fun captureRenderedFrame(gl: GL10, requestedRect: Rect?): Bitmap {
        val surfaceWidth = width
        val surfaceHeight = height
        val captureRect = requestedRect?.let { Rect(it) }
            ?: Rect(0, 0, surfaceWidth, surfaceHeight)
        require(surfaceWidth > 0 && surfaceHeight > 0 && captureRect.left >= 0 &&
                captureRect.top >= 0 && captureRect.right <= surfaceWidth &&
                captureRect.bottom <= surfaceHeight && captureRect.width() > 0 &&
                captureRect.height() > 0) { "Invalid capture rectangle" }

        val sourceWidth = captureRect.width()
        val sourceHeight = captureRect.height()
        // The source is the final window framebuffer. Some shaders derive visible
        // scanline/mask detail from output resolution, so retain the old 480x320
        // capture size for shader output. The default path can use the smaller GBA
        // resolution without changing its appearance.
        val maxOutputWidth = if (shader === ShaderConfig.Default) {
            PAUSE_PREVIEW_WIDTH
        } else {
            SHADER_PAUSE_PREVIEW_WIDTH
        }
        val maxOutputHeight = if (shader === ShaderConfig.Default) {
            PAUSE_PREVIEW_HEIGHT
        } else {
            SHADER_PAUSE_PREVIEW_HEIGHT
        }
        val outputScale = minOf(
            1f,
            maxOutputWidth.toFloat() / sourceWidth,
            maxOutputHeight.toFloat() / sourceHeight
        )
        val outputWidth = (sourceWidth * outputScale).toInt().coerceAtLeast(1)
        val outputHeight = (sourceHeight * outputScale).toInt().coerceAtLeast(1)
        val rowsPerBatch = minOf(CAPTURE_ROWS_PER_BATCH, sourceHeight)
        val batchPixelCount = sourceWidth.toLong() * rowsPerBatch.toLong()
        require(batchPixelCount <= Int.MAX_VALUE / RGBA_BYTES_PER_PIXEL) {
            "Invalid capture dimensions"
        }
        val requiredBufferBytes = batchPixelCount.toInt() * RGBA_BYTES_PER_PIXEL
        val buffer = capturePixelBuffer
            ?.takeIf { it.capacity() >= requiredBufferBytes }
            ?: ByteBuffer.allocateDirect(requiredBufferBytes).also {
                capturePixelBuffer = it
            }
        val bitmapIndex = nextPausePreviewBitmap
        nextPausePreviewBitmap = (nextPausePreviewBitmap + 1) % pausePreviewBitmaps.size
        val preview = pausePreviewBitmaps[bitmapIndex]
            ?.takeIf {
                !it.isRecycled && it.width == outputWidth && it.height == outputHeight
            }
            ?: Bitmap.createBitmap(
                outputWidth,
                outputHeight,
                Bitmap.Config.RGB_565
            ).also {
                pausePreviewBitmaps[bitmapIndex] = it
            }
        val rowPixels = IntArray(outputWidth)
        val sourceX = IntArray(outputWidth) { outputX ->
            ((outputX + 0.5f) * sourceWidth / outputWidth)
                .toInt().coerceAtMost(sourceWidth - 1)
        }

        val previousFramebuffer = IntArray(1)
        val previousPackAlignment = IntArray(1)
        GLES20.glGetIntegerv(GLES20.GL_FRAMEBUFFER_BINDING, previousFramebuffer, 0)
        GLES20.glGetIntegerv(GLES20.GL_PACK_ALIGNMENT, previousPackAlignment, 0)
        try {
            // Always read the window surface. A custom multi-pass shader may
            // leave one of its intermediate FBOs bound.
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
            gl.glPixelStorei(GL10.GL_PACK_ALIGNMENT, 1)
            var sourceTop = 0
            var outputY = 0
            while (sourceTop < sourceHeight) {
                val rowCount = minOf(rowsPerBatch, sourceHeight - sourceTop)
                val byteCount = sourceWidth * rowCount * RGBA_BYTES_PER_PIXEL
                buffer.clear()
                buffer.limit(byteCount)
                gl.glReadPixels(
                    captureRect.left,
                    surfaceHeight - captureRect.top - sourceTop - rowCount,
                    sourceWidth,
                    rowCount,
                    GL10.GL_RGBA,
                    GL10.GL_UNSIGNED_BYTE,
                    buffer
                )
                buffer.position(0)

                while (outputY < outputHeight) {
                    val sampledSourceY = ((outputY + 0.5f) * sourceHeight /
                            outputHeight).toInt().coerceAtMost(sourceHeight - 1)
                    if (sampledSourceY >= sourceTop + rowCount) break
                    if (sampledSourceY < sourceTop) {
                        outputY++
                        continue
                    }
                    val sourceRowInBuffer = rowCount - 1 - (sampledSourceY - sourceTop)
                    for (outputX in 0 until outputWidth) {
                        val sourceOffset = (sourceRowInBuffer * sourceWidth +
                                sourceX[outputX]) * RGBA_BYTES_PER_PIXEL
                        val red = buffer.get(sourceOffset).toInt() and 0xFF
                        val green = buffer.get(sourceOffset + 1).toInt() and 0xFF
                        val blue = buffer.get(sourceOffset + 2).toInt() and 0xFF
                        rowPixels[outputX] = 0xFF000000.toInt() or
                                (red shl 16) or (green shl 8) or blue
                    }
                    preview.setPixels(rowPixels, 0, outputWidth, 0, outputY, outputWidth, 1)
                    outputY++
                }
                sourceTop += rowCount
            }
            return preview
        }
        finally {
            gl.glPixelStorei(GL10.GL_PACK_ALIGNMENT, previousPackAlignment[0])
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, previousFramebuffer[0])
        }
    }

    private fun takeCaptureAndPauseRequest(): CaptureAndPauseRequest? {
        val request = captureAndPauseRequest.getAndSet(null) ?: return null
        request.timeoutRunnable?.let { mainHandler.removeCallbacks(it) }
        return request
    }

    private class CaptureAndPauseRequest(
        val captureRect: Rect?,
        val block: (Bitmap?) -> Unit,
        val needPause: Boolean
    ) {
        var timeoutRunnable: Runnable? = null
    }

    // These functions are called from the GL thread.
    private fun initializeCore() = catchExceptions {
        if (isGameLoaded) return@catchExceptions
        when {
            data.gameFilePath != null -> LibretroDroid.loadGameFromPath(data.gameFilePath!!)
            data.gameFileBytes != null -> {
                LibretroDroid.loadGameFromBytes(data.gameFileBytes!!)
                // The native core owns its ROM image after loadGameFromBytes returns.
                // Retaining the application byte array keeps another full ROM in the
                // managed heap and raises ART pressure when the third/fourth console
                // is created.
                data.gameFileBytes = null
            }
            data.gameVirtualFiles.isNotEmpty() -> LibretroDroid.loadGameFromVirtualFiles(data.gameVirtualFiles.map { DetachedVirtualFile(it.virtualPath, it.fileDescriptor.detachFd()) })
        }
        data.saveRAMState?.let {
            if (!coreUnserializeSRAM(it)) {
                throw RetroException(LibretroDroid.ERROR_SERIALIZATION)
            }
            data.saveRAMState = null
        }
        LibretroDroid.onSurfaceCreated()
        initializePrimaryRomConsole()
        isGameLoaded = true

        KtUtils.runOnUIThread {
            lifecycle?.addObserver(RenderLifecycleObserver())
        }
    }

    fun reset2(gameFileBytes: ByteArray,saveRAMState: ByteArray?) = catchExceptions {
        check(ownsNativeSession())
        LibretroDroid.reloadGameFromBytes(gameFileBytes)
        saveRAMState?.let {
            check(coreUnserializeSRAM(it)) { "Cannot restore battery save" }
        }
        initializePrimaryRomConsole()
    }

    /**
     * Reloads the primary game and restores its runtime state as one GL-thread
     * operation, so no emulation step can run against a partially rebuilt core.
     * A null result means that the GL task itself could not complete; false
     * means the core was reloaded but rejected the supplied runtime state.
     */
    fun reloadGameFromBytesAndState(
        gameFileBytes: ByteArray,
        saveRAMState: ByteArray?,
        saveState: ByteArray?,
        clearCheats: Boolean
    ): Boolean? = runOnGLThreadOrNull {
        LibretroDroid.reloadGameFromBytes(gameFileBytes)
        saveRAMState?.takeIf { it.isNotEmpty() }?.let {
            check(coreUnserializeSRAM(it)) { "Cannot restore battery save" }
        }
        initializePrimaryRomConsole()
        val stateRestored = saveState?.takeIf { it.isNotEmpty() }?.let {
            LibretroDroid.unserializeState(it)
        } ?: true
        if (clearCheats) {
            LibretroDroid.resetCheat()
        }
        stateRestored
    }

    /** Destructive reloads must finish before the caller releases its ROM-session lock. */
    suspend fun reloadGameFromBytesAndStateAwait(
        gameFileBytes: ByteArray,
        saveRAMState: ByteArray?,
        saveState: ByteArray?,
        clearCheats: Boolean
    ): Boolean = awaitCoreOperation {
        LibretroDroid.reloadGameFromBytes(gameFileBytes)
        initializePrimaryRomConsole()
        saveRAMState?.takeIf { it.isNotEmpty() }?.let { check(coreUnserializeSRAM(it)) }
        val restored = saveState?.takeIf { it.isNotEmpty() }?.let {
            LibretroDroid.unserializeState(it)
        } ?: true
        if (clearCheats) LibretroDroid.resetCheat()
        restored
    }

    /** Actual loaded BIOS bytes and execution mode, not the current settings-file value. */
    fun getBiosIdentity(): Long = runOnGLThread(-1L) { LibretroDroid.getBiosIdentity() }

    suspend fun resetAndRestoreSramAwait(saveRAMState: ByteArray?): Boolean =
        awaitCoreOperation {
            check(isGameLoaded)
            LibretroDroid.reset()
            saveRAMState?.takeIf { it.isNotEmpty() }?.let { check(coreUnserializeSRAM(it)) }
            true
        }

    suspend fun unserializeStateAwait(console: RomConsole?, state: ByteArray): Boolean =
        awaitCoreOperation {
            if (state.isEmpty()) false
            else if (console != null) unserializeRomConsoleState(console, state)
            else LibretroDroid.unserializeState(state)
        }

    suspend fun resetLocalConsoleAwait(console: RomConsole): Pair<Boolean, Boolean> =
        awaitCoreOperation {
            if (romConsoles.getOrNull(console.playerId) !== console)
                return@awaitCoreOperation false to true
            val wasLinked = LibretroDroid.getLinkNetplayStatus() == RomConsole.LINK_STATUS_ACTIVE
            if (wasLinked) LibretroDroid.stopLocalLink()
            var linkRestored = true
            val reset = try {
                LibretroDroid.resetRomConsole(console.nativeHandle)
            } finally {
                if (wasLinked) {
                    linkRestored = LibretroDroid.startLocalLink(romConsoles.map { it.nativeHandle }.toLongArray())
                    if (linkRestored)
                        linkRestored = LibretroDroid.setForegroundRomConsole(console.nativeHandle)
                }
            }
            reset to linkRestored
        }

    private inner class CoreOperation<T>(
        private val continuation: Continuation<T>,
        private val action: () -> T
    ) : Runnable {
        private val claimed = AtomicBoolean(false)

        override fun run() {
            if (!claimed.compareAndSet(false, true)) return
            complete(runCatching {
                check(ownsNativeSession())
                action()
            })
        }

        fun abort(error: Throwable = CancellationException("Native session destroyed")) {
            // Destruction may discard a queued task, never an executing reload.
            if (claimed.compareAndSet(false, true)) complete(Result.failure(error))
        }

        private fun complete(result: Result<T>) {
            synchronized(coreOperationLock) { coreOperations.remove(this) }
            continuation.resumeWith(result)
        }
    }

    private suspend fun <T> awaitCoreOperation(action: () -> T): T {
        currentCoroutineContext().ensureActive()
        val result = withContext(NonCancellable) {
            suspendCoroutine<T> { continuation ->
                val task = CoreOperation(continuation, action)
                val accepted = synchronized(coreOperationLock) {
                    if (!ownsNativeSession() || glThreadExited) false
                    else { coreOperations.add(task); true }
                }
                if (!accepted) task.abort()
                else try { queueEvent(task) } catch (e: Exception) { task.abort(e) }
            }
        }
        // Cancellation is observed only after native mutation/rollback finishes,
        // so the Activity cannot release its session lock while GL still reloads.
        currentCoroutineContext().ensureActive()
        return result
    }

    enum class SettingsReloadResult {
        APPLIED, UNCHANGED, ROLLED_BACK, RECOVERY_FAILED
    }

    /**
     * A paused, standalone GBA only. Backup, apply and rollback are one GL task.
     * Unlike a timed FutureTask, cancellation cannot report failure while a
     * destructive reload is still running. Once started, it finishes rollback
     * even if the awaiting Activity has meanwhile been destroyed.
     */
    suspend fun reloadGameForSettings(
        gameFileBytes: ByteArray,
        saveRAMState: ByteArray?,
        saveState: ByteArray,
        variables: Array<Variable>
    ): SettingsReloadResult = awaitCoreOperation {
        reloadGameForSettingsOnGL(gameFileBytes,saveRAMState,saveState,variables)
    }

    private fun reloadGameForSettingsOnGL(
        gameFileBytes: ByteArray,
        saveRAMState: ByteArray?,
        saveState: ByteArray,
        variables: Array<Variable>
    ): SettingsReloadResult {
        var oldRom: ByteArray? = null
        var oldBios: ByteArray? = null
        var oldBiosIdentity = -1L
        var oldVariables: Array<Variable>? = null
        var backupDirectory: File? = null
        var reloadStarted = false

        fun readImage(address: Int, minSize: Int, maxSize: Int): ByteArray {
            val size = LibretroDroid.readMemoryData(address,ByteArray(0))
            check(size in minSize..maxSize) { "Invalid GBA memory image" }
            return ByteArray(size).also {
                check(LibretroDroid.readMemoryData(address,it) == size) { "Incomplete GBA memory image" }
            }
        }

        fun restoreRuntime() {
            initializePrimaryRomConsole()
            saveRAMState?.takeIf { it.isNotEmpty() }?.let { check(coreUnserializeSRAM(it)) }
            check(LibretroDroid.unserializeState(saveState)) { "Runtime state rejected" }
            LibretroDroid.resetCheat()
        }

        try {
            check(!isEmulationReady && romConsoles.size == 1 && saveState.isNotEmpty())
            oldVariables = LibretroDroid.getVariables()
            // Snapshot the loaded IPS/UPS result, but not live ROM-cheat writes.
            // An old core without this extension must leave the game untouched.
            oldRom = requireNotNull(LibretroDroid.copyUnpatchedRom())
            oldBios = readImage(0,0x4000,0x4000)
            oldBiosIdentity = LibretroDroid.getBiosIdentity()
            check(oldBiosIdentity >= 0L)
            val directory = File(context.cacheDir,"rom-settings-${UUID.randomUUID()}")
            check(directory.mkdir()) { "Cannot prepare BIOS rollback directory" }
            backupDirectory = directory
            File(directory,"gba_bios.bin").writeBytes(oldBios)

            variables.forEach { LibretroDroid.updateVariable(it) }
            reloadStarted = true
            LibretroDroid.reloadGameFromBytes(gameFileBytes)
            restoreRuntime()
            return SettingsReloadResult.APPLIED
        } catch (_: Throwable) {
            if (!reloadStarted) {
                oldVariables?.forEach { LibretroDroid.updateVariable(it) }
                return SettingsReloadResult.UNCHANGED
            }
            return try {
                val rollbackVariables = requireNotNull(oldVariables)
                val rollbackRom = requireNotNull(oldRom)
                rollbackVariables.forEach { LibretroDroid.updateVariable(it) }
                // Equal bytes alone are not equal execution environments:
                // fullBios decides whether SWIs run BIOS code or the HLE path.
                if ((oldBiosIdentity and (1L shl 32)) != 0L) {
                    LibretroDroid.updateVariable(Variable("mgba_use_bios","ON"))
                    LibretroDroid.reloadGameFromBytesWithSystemDirectory(rollbackRom,requireNotNull(backupDirectory).absolutePath)
                } else {
                    LibretroDroid.updateVariable(Variable("mgba_use_bios","OFF"))
                    LibretroDroid.reloadGameFromBytes(rollbackRom)
                }
                check(LibretroDroid.getBiosIdentity() == oldBiosIdentity)
                rollbackVariables.forEach { LibretroDroid.updateVariable(it) }
                restoreRuntime()
                SettingsReloadResult.ROLLED_BACK
            } catch (_: Throwable) {
                SettingsReloadResult.RECOVERY_FAILED
            }
        } finally {
            // Only this operation's two explicitly owned temporary paths.
            backupDirectory?.let {
                runCatching {
                    File(it,"gba_bios.bin").delete()
                    it.delete()
                }
            }
        }
    }

    fun rebuildRomConsoles(
        romBytes: Array<ByteArray>,
        saveRAMStates: Array<ByteArray?>,
        foregroundPosition: Int,
        consoleStates: Array<ByteArray?> = arrayOfNulls(romBytes.size),
        startLocalLink: Boolean = true
    ): List<RomConsole>? = rebuildRomConsoles(
        romBytes.size,
        { position -> romBytes[position] },
        saveRAMStates,
        foregroundPosition,
        consoleStates,
        startLocalLink
    )

    fun rebuildRomConsoles(
        romCount: Int,
        romBytesProvider: (Int) -> ByteArray?,
        saveRAMStates: Array<ByteArray?>,
        foregroundPosition: Int,
        consoleStates: Array<ByteArray?> = arrayOfNulls(romCount),
        startLocalLink: Boolean = true
    ): List<RomConsole>? {
        if (romCount !in 1..4 || romCount != saveRAMStates.size ||
            romCount != consoleStates.size || foregroundPosition !in 0 until romCount) {
            return null
        }
        try {
            // Obtain one ROM at a time outside the GL task. Each task returns before
            // the next provider call, allowing a large Java ByteArray to become
            // unreachable instead of retaining every rebuilt ROM on one GL stack.
            val primaryReady = romBytesProvider(0)
                ?.takeIf { it.isNotEmpty() }
                ?.let { primaryRomBytes ->
                    runOnGLThread(false) {
                        LibretroDroid.reloadGameFromBytes(primaryRomBytes)
                        saveRAMStates[0]?.takeIf { it.isNotEmpty() }?.let {
                            if (!coreUnserializeSRAM(it)) return@runOnGLThread false
                        }
                        initializePrimaryRomConsole()
                        if (romConsoles.size != 1) {
                            return@runOnGLThread false
                        }
                        consoleStates[0]?.takeIf { it.isNotEmpty() }?.let {
                            if (!LibretroDroid.unserializeRomConsoleState(
                                    romConsoles[0].nativeHandle,
                                    it
                                )) {
                                return@runOnGLThread false
                            }
                        }
                        true
                    }
                }
                ?: false
            if (!primaryReady) {
                return null
            }

            for (position in 1 until romCount) {
                val consoleReady = romBytesProvider(position)
                    ?.takeIf { it.isNotEmpty() }
                    ?.let { currentRomBytes ->
                        runOnGLThread(false) {
                            val handle = LibretroDroid.createRomConsoleHandle(
                                currentRomBytes,
                                saveRAMStates[position]
                            )
                            if (handle == 0L) {
                                return@runOnGLThread false
                            }
                            val crc32 = LibretroDroid.getRomConsoleCrc32(handle)
                            if (crc32 < 0L) {
                                return@runOnGLThread false
                            }
                            RomConsole(this, handle, position, crc32).also {
                                romConsoles.add(it)
                            }
                            consoleStates[position]?.takeIf { it.isNotEmpty() }?.let {
                                if (!LibretroDroid.unserializeRomConsoleState(handle, it)) {
                                    return@runOnGLThread false
                                }
                            }
                            true
                        }
                    }
                    ?: false
                if (!consoleReady) {
                    return null
                }
            }

            return runOnGLThreadOrNull {
                if (startLocalLink && romConsoles.size > 1) {
                    if (!LibretroDroid.startLocalLink(
                            romConsoles.map { it.nativeHandle }.toLongArray()
                        )) {
                        return@runOnGLThreadOrNull null
                    }
                    if (foregroundPosition != 0 &&
                        !LibretroDroid.setForegroundRomConsole(
                            romConsoles[foregroundPosition].nativeHandle
                        )) {
                        return@runOnGLThreadOrNull null
                    }
                }
                romConsoles.toList()
            }
        }
        catch (e: Exception) {
            /* LINK_DIAGNOSTIC_DISABLED: Log.e(TAG_LOG, "Failed to rebuild ROM consoles", e) */
            return null
        }
    }

    private fun coreUnserializeSRAM(sram: ByteArray): Boolean {
        return LibretroDroid.unserializeSRAM(sram)
    }

    private fun initializePrimaryRomConsole() {
        romConsoles.clear()
        primaryRomConsole = null
        if (!LibretroDroid.hasRomConsoleSupport()) return
        val handle = LibretroDroid.getPrimaryRomConsoleHandle()
        if (handle == 0L) return
        val crc32 = LibretroDroid.getRomConsoleCrc32(handle)
        if (crc32 < 0L) return
        RomConsole(this, handle, 0, crc32).also {
            romConsoles.add(it)
            primaryRomConsole = it
        }
    }

    private fun catchExceptions(block: () -> Unit) {
        try {
            if (isAborted || isDestroyed) return
            block()
        } catch (e: RetroException) {
            GlobalScope.launch {
                retroGLIssuesErrors.emit(e.errorCode)
            }
            isAborted = true
        } catch (e: Exception) {
            Log.e(TAG_LOG, "Error in GLRetroView", e)
            GlobalScope.launch {
                retroGLIssuesErrors.emit(LibretroDroid.ERROR_GENERIC)
            }
        }
    }

    private fun <T> runOnGLThread(defValue:T,block: () -> T): T {
        return runOnGLThreadOrNull(block) ?: defValue
    }

    private fun <T> runOnGLThreadOrNull(block: () -> T): T? {
        if (!ownsNativeSession()) return null
        if (Thread.currentThread() == glThread.get()) {
            return block()
        }

        if (isDestroyed)
            return null

        val task = FutureTask<T?> { if (ownsNativeSession()) block() else null }

        return try {
            queueEvent(task)
            task.get(2000, TimeUnit.MILLISECONDS)
        }
        catch (e: TimeoutException) {
            // 防止超时后任务又在 GL 线程执行。
            Log.w(TAG_LOG, "Timed out waiting for GL thread after 2000ms",e)
            task.cancel(false)
            null
        }
        catch (e: InterruptedException) {
            Log.w(TAG_LOG,"Interrupted while waiting for GL thread",e)
            task.cancel(false)
            Thread.currentThread().interrupt()
            null
        }
        catch (e: ExecutionException) {
            Log.w(TAG_LOG,"Execution exception while waiting for GL thread",e)
            null
        }
    }

    private fun runOnGLThreadVoid(block: () -> Unit) {
        if (!ownsNativeSession())
            return

        val safeBlock = {
            catchExceptions(block)
        }

        if (Thread.currentThread() === glThread.get()) {
            safeBlock()
        }
        else {
            queueEvent {
                if (ownsNativeSession())
                    safeBlock()
            }
        }
    }

    private fun buildShader(config: ShaderConfig): GLRetroShader {
        return when (config) {
            is ShaderConfig.Default -> GLRetroShader(LibretroDroid.SHADER_DEFAULT)
            is ShaderConfig.CRT -> GLRetroShader(LibretroDroid.SHADER_CRT)
            is ShaderConfig.LCD -> GLRetroShader(LibretroDroid.SHADER_LCD)
            is ShaderConfig.Sharp -> GLRetroShader(LibretroDroid.SHADER_SHARP)
            is ShaderConfig.CUT -> GLRetroShader(
                LibretroDroid.SHADER_UPSCALE_CUT,
                buildParams(
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_USE_DYNAMIC_BLEND to toParam(config.useDynamicBlend),
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_BLEND_MIN_CONTRAST_EDGE to toParam(config.blendMinContrastEdge),
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_BLEND_MAX_CONTRAST_EDGE to toParam(config.blendMaxContrastEdge),
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_BLEND_MIN_SHARPNESS to toParam(config.blendMinSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_BLEND_MAX_SHARPNESS to toParam(config.blendMaxSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_STATIC_BLEND_SHARPNESS to toParam(config.staticSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_EDGE_USE_FAST_LUMA to toParam(config.edgeUseFastLuma),
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_EDGE_MIN_VALUE to toParam(config.edgeMinValue),
                    LibretroDroid.SHADER_UPSCALE_CUT_PARAM_EDGE_MIN_CONTRAST to toParam(config.edgeMinContrast),
                )
            )
            is ShaderConfig.CUT2 -> GLRetroShader(
                LibretroDroid.SHADER_UPSCALE_CUT2,
                buildParams(
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_USE_DYNAMIC_BLEND to toParam(config.useDynamicBlend),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_BLEND_MIN_CONTRAST_EDGE to toParam(config.blendMinContrastEdge),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_BLEND_MAX_CONTRAST_EDGE to toParam(config.blendMaxContrastEdge),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_BLEND_MIN_SHARPNESS to toParam(config.blendMinSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_BLEND_MAX_SHARPNESS to toParam(config.blendMaxSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_STATIC_BLEND_SHARPNESS to toParam(config.staticSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_EDGE_USE_FAST_LUMA to toParam(config.edgeUseFastLuma),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_SOFT_EDGES_SHARPENING to toParam(config.softEdgesSharpening),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_SOFT_EDGES_SHARPENING_AMOUNT to toParam(config.softEdgesSharpeningAmount),
                    LibretroDroid.SHADER_UPSCALE_CUT2_PARAM_HARD_EDGES_SEARCH_MAX_ERROR to toParam(config.hardEdgesSearchMaxError),
                )
            )
            is ShaderConfig.CUT3 -> GLRetroShader(
                LibretroDroid.SHADER_UPSCALE_CUT3,
                buildParams(
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_USE_DYNAMIC_BLEND to toParam(config.useDynamicBlend),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_BLEND_MIN_CONTRAST_EDGE to toParam(config.blendMinContrastEdge),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_BLEND_MAX_CONTRAST_EDGE to toParam(config.blendMaxContrastEdge),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_BLEND_MIN_SHARPNESS to toParam(config.blendMinSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_BLEND_MAX_SHARPNESS to toParam(config.blendMaxSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_STATIC_BLEND_SHARPNESS to toParam(config.staticSharpness),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_EDGE_USE_FAST_LUMA to toParam(config.edgeUseFastLuma),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_SOFT_EDGES_SHARPENING to toParam(config.softEdgesSharpening),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_SOFT_EDGES_SHARPENING_AMOUNT to toParam(config.softEdgesSharpeningAmount),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_HARD_EDGES_SEARCH_MAX_ERROR to toParam(config.hardEdgesSearchMaxError),
                    LibretroDroid.SHADER_UPSCALE_CUT3_PARAM_HARD_EDGES_SEARCH_MAX_DISTANCE to toParam(config.hardEdgesSearchMaxDistance),
                )
            )
            is ShaderConfig.Custom -> GLRetroShader(
                LibretroDroid.SHADER_CUSTOM,
                buildParams(
                    LibretroDroid.SHADER_CUSTOM_PARAM_VERTEX to config.vertex,
                    LibretroDroid.SHADER_CUSTOM_PARAM_FRAGMENT to config.fragment,
                    LibretroDroid.SHADER_CUSTOM_PARAM_LINEAR_TEXTURE to if (config.linearTexture) "1" else "0",
                    // Float.toString() is locale-independent and preserves values such as
                    // 5.625. Rounding to two decimals changes a 1350x900 pass into 1351x901.
                    LibretroDroid.SHADER_CUSTOM_PARAM_SCALE to config.scale.toString(),
                    LibretroDroid.SHADER_CUSTOM_PARAM_USE_INTERMEDIATE_FRAMEBUFFER to
                        if (config.useIntermediateFramebuffer) "1" else "0",
                    LibretroDroid.SHADER_CUSTOM_PARAM_REPEAT_SOURCE_TEXTURE to
                        if (config.repeatSourceTexture) "1" else "0",
                    LibretroDroid.SHADER_CUSTOM_PARAM_HISTORY_COUNT to config.historyCount.toString()
                ),
                config.textures
            )

        }
    }

    private fun toParam(param: Float): String {
        return param.toString()
    }

    private fun toParam(param: Boolean): String {
        return if (param) {
            "1"
        } else {
            "0"
        }
    }

    private fun toParam(param: Int): String {
        return param.toString()
    }

    private fun buildParams(vararg pairs: Pair<String, String?>): Map<String, String> {
        return pairs
            .filter { (key, value) -> value != null }
            .associate { (key, value) -> key to value!! }
    }

    /** This function gets called from the jni side.*/
    private fun sendRumbleEvent(port: Int, strengthWeak: Float, strengthStrong: Float) {
        lifecycle?.coroutineScope?.launch {
            rumbleEventsSubject.emit(RumbleEvent(port, strengthWeak, strengthStrong))
        }
    }

    private fun sendSaveDataSyncedEvent(){
        lifecycle?.coroutineScope?.launch {
            saveDataSyncedEventsSubject.emit(Unit)
        }
    }

    private fun refreshAspectRatio() {
        runOnGLThreadVoid {
            LibretroDroid.refreshAspectRatio()
        }
    }

    sealed class GLRetroEvents {
        object FrameRendered: GLRetroEvents()
        object SurfaceCreated: GLRetroEvents()
    }

    companion object {
        private val nativeSessionLock = Any()
        private val nativeSessionOwner = AtomicReference<GLRetroView?>()
        private val pendingNativeSessions = mutableListOf<GLRetroView>()
        private val TAG_LOG = GLRetroView::class.java.simpleName
        private const val CAPTURE_AND_PAUSE_TIMEOUT_MS = 200L
        private const val CAPTURE_ROWS_PER_BATCH = 64
        private const val RGBA_BYTES_PER_PIXEL = 4
        private const val PAUSE_PREVIEW_WIDTH = 240
        private const val PAUSE_PREVIEW_HEIGHT = 160
        private const val SHADER_PAUSE_PREVIEW_WIDTH = 480
        private const val SHADER_PAUSE_PREVIEW_HEIGHT = 320
        private const val DEFAULT_LINK_INPUT_DELAY_FRAMES = 2
        private const val MIN_LINK_INPUT_DELAY_FRAMES = 2
        private const val MAX_LINK_INPUT_DELAY_FRAMES = 30

        const val MOTION_SOURCE_DPAD = LibretroDroid.MOTION_SOURCE_DPAD
        const val MOTION_SOURCE_ANALOG_LEFT = LibretroDroid.MOTION_SOURCE_ANALOG_LEFT
        const val MOTION_SOURCE_ANALOG_RIGHT = LibretroDroid.MOTION_SOURCE_ANALOG_RIGHT
        const val MOTION_SOURCE_POINTER = LibretroDroid.MOTION_SOURCE_POINTER

        const val ERROR_LOAD_LIBRARY = LibretroDroid.ERROR_LOAD_LIBRARY
        const val ERROR_LOAD_GAME = LibretroDroid.ERROR_LOAD_GAME
        const val ERROR_GL_NOT_COMPATIBLE = LibretroDroid.ERROR_GL_NOT_COMPATIBLE
        const val ERROR_SERIALIZATION = LibretroDroid.ERROR_SERIALIZATION
        const val ERROR_CHEAT = LibretroDroid.ERROR_CHEAT
        const val ERROR_GENERIC = LibretroDroid.ERROR_GENERIC

        private val TOUCH_EVENT_OUTSIDE = PointF(-10f, 10f)
    }
}
