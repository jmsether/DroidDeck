package com.droiddeck.launcher.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DeviceReportPowerVrTest {
    private val defaults = mapOf("A" to "1", "B" to null)

    @Test fun unknownFactsReadUnknownNeverAGuess() {
        val rows = DeviceReport.powerVrRows(emptyMap(), 3_185_046, defaults, true).toMap()
        assertEquals("unknown", rows["Vulkan device"])
        assertEquals("unknown", rows["Driver"])
        assertEquals("unknown", rows["Driver BC feature"])
        assertTrue(rows.getValue("System driver").endsWith("(3185046 bytes)"))
    }

    @Test fun driverFactsAndGameDefaultsAreListed() {
        val vk = mapOf("device" to "PowerVR D-Series DXT-48-1536 MC1", "api" to "1.4.317",
            "driver_name" to "PowerVR", "driver_info" to "25.3@6908880", "texture_bc" to "no")
        val rows = DeviceReport.powerVrRows(vk, 1, defaults, true).toMap()
        assertEquals("PowerVR D-Series DXT-48-1536 MC1 · API 1.4.317", rows["Vulkan device"])
        assertEquals("PowerVR 25.3@6908880", rows["Driver"])
        assertEquals("no", rows["Driver BC feature"])
        assertEquals("A=1 -B", rows["Game defaults"])
        assertEquals("session.log: HYBRIS_DRM_PROPERTIES and HYBRIS_BC lines", rows["Compat layer"])
    }

    @Test fun anImportedLinuxDriverMeansNoDefaultsAndNoCompatLayer() {
        val rows = DeviceReport.powerVrRows(emptyMap(), 1, defaults, false).toMap()
        assertTrue(rows.getValue("Game defaults").startsWith("none"))
        assertTrue(rows.getValue("Compat layer").startsWith("not loaded"))
    }

    @Test fun theVulkanFailureReasonIsKept() {
        val rows = DeviceReport.powerVrRows(mapOf("error" to "vkCreateInstance failed"), 1, defaults, true).toMap()
        assertEquals("unknown (vkCreateInstance failed)", rows["Vulkan device"])
    }

    @Test fun keysFitTheReportColumn() {
        DeviceReport.powerVrRows(emptyMap(), 0, defaults, true).forEach { (key, _) -> assertTrue(key, key.length < 24) }
    }
}
