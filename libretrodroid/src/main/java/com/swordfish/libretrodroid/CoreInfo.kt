package com.swordfish.libretrodroid

class CoreInfo(
    /** library_name */
    val name: String = "",
    /** library_version */
    val version: String = "",
    /** valid_extensions */
    val types: String = "",
    /** need_fullpath */
    val needFullPath: Boolean = false,
    /** block_extract */
    val blockExtract: Boolean = false
)
