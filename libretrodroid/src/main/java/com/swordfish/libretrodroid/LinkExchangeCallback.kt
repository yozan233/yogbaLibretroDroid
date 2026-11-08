package com.swordfish.libretrodroid

/** Called synchronously on the emulation/GL thread. */
interface LinkExchangeCallback {
    fun onLinkExchange(type: Int, args: IntArray, data: ShortArray): Int
}
