package com.droiddeck.launcher.gpu;

import android.content.Context;
import android.os.Build;
import com.droiddeck.launcher.core.FileUtils;
import com.droiddeck.launcher.core.TarZst;
import java.io.File;
import java.io.IOException;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.UUID;
import org.json.JSONObject;

/** Isolated glibc-to-Android Vulkan bridge. It does not replace the rootfs's Mesa libraries. */
public final class SystemVulkanDriver {
    private static final String ASSET = "hybris/runtime.tzst";
    /** Public for the session report (core.DeviceReport). */
    public static final String HAL = "/vendor/lib64/hw/vulkan.powervr.so";
    private static final String[] REQUIRED = {
        "lib/libhybris-vulkan-icd.so.0", "lib/libhybris-common.so.1",
        "lib/libhybris/linker/q.so", "lib/libVkLayer_hybris_compat.so"
    };

    private SystemVulkanDriver() {}

    public static boolean isPowerVr() { return isPowerVr(new File("/")); }

    /** {@code root} is "/" on a device; a test points it at a directory holding a fake /vendor. */
    public static boolean isPowerVr(File root) { return new File(root, HAL).isFile(); }

    public static boolean usesDefault(String choice) {
        return usesDefault(isPowerVr(), choice);
    }

    static boolean usesDefault(boolean powerVr, String choice) {
        return powerVr && (choice == null || choice.isEmpty());
    }

    /** APK-versioned directories stay intact while an older session still has libraries mapped. */
    public static synchronized File prepare(Context context) throws IOException {
        File target = runtimeDirectory(context);
        File base = target.getParentFile();
        if (complete(target)) return target;
        if (!base.isDirectory() && !base.mkdirs()) throw new IOException("Cannot create system Vulkan directory");
        File staged = new File(base, ".stage-" + UUID.randomUUID());
        try {
            if (!TarZst.extractAsset(context, ASSET, staged)) throw new IOException("Cannot unpack system Vulkan runtime");
            for (String name : REQUIRED)
                if (!new File(staged, name).isFile()) throw new IOException("Missing system Vulkan library: " + name);
            JSONObject library = new JSONObject();
            library.put("library_path", new File(target, REQUIRED[0]).getPath());
            library.put("api_version", "1.3.0");
            JSONObject manifest = new JSONObject();
            manifest.put("file_format_version", "1.0.0");
            manifest.put("ICD", library);
            Files.write(new File(staged, "icd.json").toPath(), manifest.toString().getBytes(StandardCharsets.UTF_8));
            if (target.exists()) throw new IOException("Incomplete system Vulkan runtime directory: " + target.getName());
            if (!staged.renameTo(target)) throw new IOException("Cannot publish system Vulkan runtime");
            return target;
        } catch (org.json.JSONException error) {
            throw new IOException("Cannot write system Vulkan manifest", error);
        } finally {
            if (staged.exists()) FileUtils.delete(staged);
        }
    }

    /** The current APK's versioned runtime path, without extracting or modifying it. */
    public static File runtimeDirectory(Context context) throws IOException {
        String version;
        try (BufferedReader input = new BufferedReader(new InputStreamReader(
                context.getAssets().open("hybris/version.txt"), StandardCharsets.US_ASCII))) {
            version = input.readLine();
        }
        if (version == null || !version.matches("[0-9a-f]{64}")) throw new IOException("Invalid system Vulkan runtime version");
        return new File(new File(context.getFilesDir(), "system_vulkan"), version);
    }

    private static boolean complete(File directory) {
        if (!new File(directory, "icd.json").isFile()) return false;
        for (String name : REQUIRED) if (!new File(directory, name).isFile()) return false;
        return true;
    }

    public static String icdPath(Context context) {
        try {
            return new File(prepare(context), "icd.json").getPath();
        } catch (IOException error) {
            throw new IllegalStateException("System Vulkan runtime is unavailable", error);
        }
    }

    public static void addEnvironment(Context context, List<String> environment) {
        File directory;
        try { directory = prepare(context); }
        catch (IOException error) { throw new IllegalStateException("System Vulkan runtime is unavailable", error); }
        environment.add("HYBRIS_ANDROID_SDK_VERSION=" + Build.VERSION.SDK_INT);
        environment.add("HYBRIS_VULKAN_HAL=" + HAL);
        environment.add("HYBRIS_LINKER_DIR=" + new File(directory, "lib/libhybris/linker").getPath());
        environment.add("HYBRIS_LD_LIBRARY_PATH=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/system_ext/lib64");
        environment.add("VK_LAYER_PATH=" + new File(directory, "lib").getPath());
        environment.add("VK_INSTANCE_LAYERS=VK_LAYER_HYBRIS_compat");
        // The Android driver has a render node, but no KMS primary node. SDL presents
        // through the wrapper's Wayland Vulkan swapchain instead of a KMS-style path.
        environment.add("BL_GAMESCOPE_BACKEND=sdl");
        // The Android pvr kernel accepts Vulkan DMA-BUF imports but rejects PRIME GEM handles.
        environment.add("BL_POWERVR_DRM_HANDLES=1");
        // Inner Gamescope/Xwayland lacks the bridge's Android-buffer WSI protocols.
        environment.add("BL_STEAM_GL_PRESENT=1");
    }
}
