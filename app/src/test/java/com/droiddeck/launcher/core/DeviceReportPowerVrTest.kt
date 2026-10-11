package com.droiddeck.launcher.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DeviceReportPowerVrTest {
    @Test fun unknownFactsReadUnknownNeverAGuess() {
        val rows = DeviceReport.powerVrRows(emptyMap(), 3_185_046).toMap()
        assertEquals("unknown", rows["Vulkan device"])
        assertEquals("unknown", rows["Driver"])
        assertEquals("unknown", rows["Driver BC feature"])
        assertTrue(rows.getValue("System driver").endsWith("(3185046 bytes)"))
    }

    @Test fun driverFactsAndGameDefaultsAreListed() {
        val vk = mapOf("device" to "PowerVR D-Series DXT-48-1536 MC1", "api" to "1.4.317",
            "driver_name" to "PowerVR", "driver_info" to "25.3@6908880", "texture_bc" to "no")
        val rows = DeviceReport.powerVrRows(vk, 1).toMap()
        assertEquals("PowerVR D-Series DXT-48-1536 MC1 · API 1.4.317", rows["Vulkan device"])
        assertEquals("PowerVR 25.3@6908880", rows["Driver"])
        assertEquals("no", rows["Driver BC feature"])
        assertEquals("HYBRIS_BC_TEXTURES=d3d DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1 -ENABLE_VK_LAYER_VALVE_steam_fossilize_1",
            rows["Game defaults"])
    }

    @Test fun keysFitTheReportColumn() {
        DeviceReport.powerVrRows(emptyMap(), 0).forEach { (key, _) -> assertTrue(key, key.length < 24) }
    }
}
