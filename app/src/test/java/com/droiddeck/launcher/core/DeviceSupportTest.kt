package com.droiddeck.launcher.core

import com.droiddeck.launcher.gpu.GpuInfo
import com.droiddeck.launcher.gpu.SystemVulkanDriver
import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

class DeviceSupportTest {
    private lateinit var root: File

    @Before fun setUp() { root = Files.createTempDirectory("device-support").toFile() }
    @After fun tearDown() { root.deleteRecursively() }

    private fun touch(path: String) { File(root, path).apply { parentFile!!.mkdirs(); createNewFile() } }

    @Test fun neitherStackCannotDraw() {
        assertFalse(GpuInfo.isAdreno(root))
        assertFalse(SystemVulkanDriver.isPowerVr(root))
        assertFalse(DeviceSupport.canDraw(root))
    }

    private fun assertAdreno() {
        assertTrue(GpuInfo.isAdreno(root))
        assertTrue(DeviceSupport.canDraw(root))
        assertFalse(SystemVulkanDriver.isPowerVr(root))
    }

    @Test fun aKgslNodeIsAnAdreno() {
        File(root, "sys/class/kgsl/kgsl-3d0").mkdirs()
        assertAdreno()
    }

    @Test fun theAdrenoDriverIsAnAdreno() {
        touch("vendor/lib64/hw/vulkan.adreno.so")
        assertAdreno()
    }

    @Test fun powerVrIsNotAnAdrenoButCanDraw() {
        touch("vendor/lib64/hw/vulkan.powervr.so")
        assertFalse(GpuInfo.isAdreno(root))
        assertTrue(SystemVulkanDriver.isPowerVr(root))
        assertTrue(DeviceSupport.canDraw(root))
    }

    @Test fun aDirectoryNamedLikeTheDriverIsNotOne() {
        File(root, "vendor/lib64/hw/vulkan.powervr.so").mkdirs()
        assertFalse(SystemVulkanDriver.isPowerVr(root))
        assertFalse(DeviceSupport.canDraw(root))
    }
}
