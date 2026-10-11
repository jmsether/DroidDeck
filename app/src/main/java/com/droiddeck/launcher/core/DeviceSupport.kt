package com.droiddeck.launcher.core

import android.content.Context
import android.os.Build
import com.droiddeck.launcher.R
import com.droiddeck.launcher.gpu.GpuInfo
import com.droiddeck.launcher.gpu.SystemVulkanDriver
import java.io.File

/**
 * Whether this device can draw a Linux session at all.
 *
 * Adreno draws with Turnip; PowerVR draws with the phone's own driver through libhybris. On Mali
 * and Xclipse the compositor gets no usable Vulkan device and a session comes up as sound over a
 * black screen - a failure with nothing in it to read. The app cannot fix that, so it says so
 * before the download rather than after it.
 */
object DeviceSupport {
    /** [root] is "/" on a device; a test points it at a directory holding a fake /sys and /vendor. */
    fun canDraw(root: File = File("/")): Boolean = GpuInfo.isAdreno(root) || SystemVulkanDriver.isPowerVr(root)

    /** The chip as the device names it, for the card that explains the refusal. */
    fun gpuName(context: Context): String {
        val soc = if (Build.VERSION.SDK_INT >= 31) Build.SOC_MODEL.takeIf { it.isNotBlank() && it != Build.UNKNOWN } else null
        return soc?.let { "$it (${Build.HARDWARE})" } ?: Build.HARDWARE.ifBlank { context.getString(R.string.gpu_this_gpu) }
    }

    /** False on the Odin 3 (CQ8725S): a low-latency Wi-Fi lock makes its WLAN firmware hang, and
     *  that firmware reboots the whole device instead of restarting Wi-Fi. */
    fun lowLatencyWifiSafe(): Boolean = Build.VERSION.SDK_INT < 31 || Build.SOC_MODEL != "CQ8725S"
}
