package org.levimc.launcher.util;

import android.content.Context;
import android.net.Uri;

import android.content.pm.PackageInfo;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

public class ApkUtils {
    public interface LibExtractProgressCallback {
        void onBytesExtracted(int bytesExtracted);
    }

    public static final String MINECRAFT_PACKAGE_NAME = "com.mojang.minecraftpe";
    public static final String UNKNOWN_VERSION_NAME = "custom";
    private static final String MINECRAFT_LIBRARY_NAME = "libminecraftpe.so";

    /**
     * Returns the version name of a Minecraft APK, or {@code null} when the file is not a
     * Minecraft APK.
     *
     * Modified APKs are accepted as well: an APK counts as Minecraft when it has the official
     * package name or when it ships {@code libminecraftpe.so}, so APKs that were repacked with a
     * different package name (or that the package parser cannot read any more) still import.
     * When no version name can be read, {@link #UNKNOWN_VERSION_NAME} is returned.
     */
    public static String readMinecraftVersionName(Context context, File apkFile) {
        PackageInfo info = null;
        try {
            info = context.getPackageManager().getPackageArchiveInfo(apkFile.getAbsolutePath(), 0);
        } catch (Exception ignored) {
        }
        boolean officialPackage = info != null && MINECRAFT_PACKAGE_NAME.equals(info.packageName);
        if (!officialPackage && !containsMinecraftLibrary(apkFile)) {
            return null;
        }
        if (info != null && info.versionName != null && !info.versionName.trim().isEmpty()) {
            return info.versionName.trim();
        }
        return UNKNOWN_VERSION_NAME;
    }

    public static boolean containsMinecraftLibrary(File apkFile) {
        try (ZipFile zip = new ZipFile(apkFile)) {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                if (isMinecraftLibraryEntry(entries.nextElement().getName())) {
                    return true;
                }
            }
        } catch (IOException ignored) {
        }
        return false;
    }

    private static boolean isMinecraftLibraryEntry(String name) {
        if (!name.startsWith("lib/") || !name.endsWith("/" + MINECRAFT_LIBRARY_NAME)) return false;
        return name.split("/").length == 3;
    }

    public static String extractMinecraftVersionNameFromUri(Context context, Uri uri) {
        if ("file".equals(uri.getScheme()) && uri.getPath() != null) {
            return toProfileName(readMinecraftVersionName(context, new File(uri.getPath())));
        }

        File tempFile = null;
        try (InputStream is = context.getContentResolver().openInputStream(uri)) {
            if (is == null) return "Error Apk";

            tempFile = new File(context.getCacheDir(), "temp_apk_" + System.currentTimeMillis() + ".apk");
            try (OutputStream os = new FileOutputStream(tempFile)) {
                copy(is, os);
            }
            return toProfileName(readMinecraftVersionName(context, tempFile));
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            if (tempFile != null && tempFile.exists()) {
                tempFile.delete();
            }
        }
        return "Error Apk";
    }

    public static String extractMinecraftVersionNameFromApksUri(Context context, Uri uri) {
        String baseVersionName = null;
        boolean basePackageReadable = false;
        boolean hasMinecraftLibrary = false;
        try (InputStream is = context.getContentResolver().openInputStream(uri)) {
            if (is == null) return "Error Apk";
            try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(is))) {
                ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    String name = entry.getName();
                    if (entry.isDirectory() || !name.endsWith(".apk")) {
                        zis.closeEntry();
                        continue;
                    }
                    boolean isBase = name.equals("base.apk") || name.endsWith("/base.apk");
                    // Splits only matter when the base APK alone does not identify Minecraft.
                    if (!isBase && (basePackageReadable || hasMinecraftLibrary)) {
                        zis.closeEntry();
                        continue;
                    }
                    File tempApkFile = new File(context.getCacheDir(), "temp_split_" + System.currentTimeMillis() + ".apk");
                    try {
                        try (OutputStream os = new FileOutputStream(tempApkFile)) {
                            copy(zis, os);
                        }
                        if (isBase) {
                            PackageInfo info = null;
                            try {
                                info = context.getPackageManager().getPackageArchiveInfo(tempApkFile.getAbsolutePath(), 0);
                            } catch (Exception ignored) {
                            }
                            if (info != null && info.versionName != null && !info.versionName.trim().isEmpty()) {
                                baseVersionName = info.versionName.trim();
                            }
                            basePackageReadable = info != null && MINECRAFT_PACKAGE_NAME.equals(info.packageName);
                        }
                        if (!hasMinecraftLibrary && containsMinecraftLibrary(tempApkFile)) {
                            hasMinecraftLibrary = true;
                        }
                    } finally {
                        tempApkFile.delete();
                    }
                    zis.closeEntry();
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
            return "Error Apk";
        }
        if (!basePackageReadable && !hasMinecraftLibrary) {
            return "Error Apk";
        }
        return toProfileName(baseVersionName != null ? baseVersionName : UNKNOWN_VERSION_NAME);
    }

    private static String toProfileName(String versionName) {
        return versionName == null ? "Error Apk" : "Minecraft_" + versionName;
    }

    private static void copy(InputStream is, OutputStream os) throws IOException {
        byte[] buffer = new byte[131072];
        int len;
        while ((len = is.read(buffer)) != -1) {
            os.write(buffer, 0, len);
        }
    }

    public static String abiToSystemLibDir(String abi) {
        if (abi == null) return "unknown";
        switch (abi) {
            case "armeabi-v7a":
                return "arm";
            case "arm64-v8a":
                return "arm64";
            default:
                return abi;
        }
    }

    public static void unzipLibsToSystemAbi(File libBaseDir, ZipInputStream zis) throws IOException {
        unzipLibsToSystemAbi(libBaseDir, zis, null);
    }

    public static void unzipLibsToSystemAbi(File libBaseDir, ZipInputStream zis, LibExtractProgressCallback callback) throws IOException {
        ZipEntry entry;
        while ((entry = zis.getNextEntry()) != null) {
            String name = entry.getName();
            if (name.startsWith("lib/") && !entry.isDirectory()) {
                String[] parts = name.split("/");
                if (parts.length < 3) continue;
                String abi = parts[1];
                String systemAbi = abiToSystemLibDir(abi);
                String soName = parts[2];
                File outFile = new File(libBaseDir, systemAbi + "/" + soName);
                File parent = outFile.getParentFile();
                if (!parent.exists()) parent.mkdirs();
                try (FileOutputStream fos = new FileOutputStream(outFile)) {
                    byte[] buffer = new byte[131072];
                    int len;
                    while ((len = zis.read(buffer)) != -1) {
                        fos.write(buffer, 0, len);
                        if (callback != null) {
                            callback.onBytesExtracted(len);
                        }
                    }
                }
                NativeImageGuard.processIfNeeded(outFile);
            }
            zis.closeEntry();
        }
    }
}
