/*
 *     Copyright (C) 2026
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 */
package com.swordfish.libretrodroid;

/**
 * Application-facing handle for one GBA core managed by the active
 * {@link GLRetroView}. All operations are dispatched to that view's GL thread.
 */
public final class RomConsole {
    public static final int LINK_STATUS_OFF = 0;
    public static final int LINK_STATUS_HANDSHAKE = 1;
    public static final int LINK_STATUS_STATE_SYNC = 2;
    public static final int LINK_STATUS_READY = 3;
    public static final int LINK_STATUS_ACTIVE = 4;
    public static final int LINK_STATUS_ROM_MISMATCH = -1;
    public static final int LINK_STATUS_PROTOCOL_ERROR = -2;
    public static final int LINK_STATUS_DISCONNECTED = -3;

    private final GLRetroView host;
    private final long nativeHandle;
    private final int playerId;
    private final long romCrc32;

    RomConsole(GLRetroView host, long nativeHandle, int playerId, long romCrc32) {
        if (host == null || nativeHandle == 0) {
            throw new IllegalArgumentException("A RomConsole requires a live host and handle");
        }
        this.host = host;
        this.nativeHandle = nativeHandle;
        this.playerId = playerId;
        this.romCrc32 = romCrc32;
    }

    /** Local console slot; LAN player IDs are defined by startLanLink argument order. */
    public int getPlayerId() {
        return playerId;
    }

    /** Returns the unsigned CRC32 of the pristine ROM as a Java long. */
    public long getRomCrc32() {
        return romCrc32;
    }

    public boolean makeForeground() {
        return host.setForegroundRomConsole(this);
    }

    public boolean isForeground() {
        return host.isForegroundRomConsole(this);
    }

    /** Resets this console without changing which console is in the foreground. */
    public boolean reset() {
        return host.resetRomConsole(this);
    }

    /** Returns this console's latest unfiltered core framebuffer. */
    public ScreenshotInfo screenshot() {
        return host.screenshotRomConsole(this);
    }

    public boolean resetCheat() {
        return host.resetRomConsoleCheat(this);
    }

    public boolean setCheat(int type, boolean enable, String code) {
        return host.setRomConsoleCheat(this, type, enable, code);
    }

    public byte[] serializeSRAM() {
        return host.serializeRomConsoleSRAM(this);
    }

    public byte[] serializeState() {
        return host.serializeRomConsoleState(this);
    }

    public boolean unserializeState(byte[] state) {
        return host.unserializeRomConsoleState(this, state);
    }

    public RomConsole createLinkedConsole(byte[] romBytes, byte[] saveRAMState) {
        return host.createRomConsole(romBytes, saveRAMState);
    }

    /** Destroys this console when it is the highest-numbered detached secondary. */
    public boolean destroy() {
        return host.destroyRomConsole(this);
    }

    public int getLinkStatus() {
        return host.getRomConsoleLinkStatus();
    }

    public boolean isLinkActive() {
        return host.isRomConsoleLinkActive();
    }

    /** Starts same-device lockstep. Consoles are ordered by their local player IDs. */
    public static boolean startLocalLink(RomConsole... consoles) {
        if (consoles == null || consoles.length < 2 || consoles[0] == null) {
            return false;
        }
        GLRetroView host = consoles[0].host;
        for (RomConsole console : consoles) {
            if (console == null || console.host != host) {
                return false;
            }
        }
        return host.startLocalLink(consoles);
    }

    /** Starts same-device lockstep and selects the visible/audio console atomically. */
    public static boolean startLocalLinkWithForeground(RomConsole foreground, RomConsole... consoles) {
        if (foreground == null || consoles == null || consoles.length < 2) {
            return false;
        }
        GLRetroView host = foreground.host;
        boolean containsForeground = false;
        for (RomConsole console : consoles) {
            if (console == null || console.host != host) {
                return false;
            }
            containsForeground |= console == foreground;
        }
        return containsForeground && host.startLocalLinkWithForeground(foreground, consoles);
    }

    /**
     * Starts LAN lockstep. {@code players} is indexed by link player ID and must
     * contain the primary console at {@code localPlayerId}. Each remote entry is
     * an application-created console using the ROM selected for that peer; the
     * ROMs of different players do not need to be identical. Native handshake
     * validation compares every peer with its corresponding console CRC.
     *
     * <p>Socket ownership transfers to native code only when this method returns
     * {@code true}. The caller must close every detached descriptor if it returns
     * {@code false}.</p>
     */
    public static boolean startLanLink(int[] socketFds, int localPlayerId, RomConsole... players) {
        if (socketFds == null || players == null || localPlayerId < 0 ||
                localPlayerId >= players.length ||
                players[localPlayerId] == null) {
            return false;
        }
        GLRetroView host = players[localPlayerId].host;
        for (RomConsole player : players) {
            if (player == null || player.host != host) {
                return false;
            }
        }
        return host.startLanLink(socketFds, localPlayerId, players);
    }

    public void stopLink() {
        host.stopLocalLink();
    }

    long getNativeHandle() {
        return nativeHandle;
    }
}
