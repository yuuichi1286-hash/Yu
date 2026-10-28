package org.levimc.launcher.core.minecraft

import android.app.Activity
import android.content.Context
import android.util.Log
import dalvik.system.DexClassLoader
import org.levimc.launcher.R
import java.io.File
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Modifier
import java.util.zip.ZipFile

/**
 * Runs what a modified Minecraft APK injects into Mojang's activity (see [ApkDexScanner]):
 * extra native libraries are loaded while the game runtime is prepared, and static entry points
 * such as a mod menu's `Start(Context)` are called once the game activity exists.
 */
object ApkModLoader {
    private const val TAG = "ApkModLoader"
    private const val CACHE_FILE_NAME = ".apk_mods"
    private const val CACHE_VERSION = 2
    private val DEX_ENTRY = Regex("classes\\d*\\.dex")

    /** Libraries that ship with the game or its SDKs; the launcher loads them itself when needed. */
    private val VANILLA_LIBRARIES = setOf(
        "c++_shared", "fmod", "MediaDecoders_Android", "minecraftpe", "HttpClient.Android",
        "PlayFabMultiplayer", "maesdk", "MallocRedirect", "pairipcore", "gxcore", "ovrfmod",
        "ovrplatformloader", "conscrypt_jni", "crashlytics", "crashlytics-common",
        "crashlytics-handler", "crashlytics-trampoline", "datastore_shared_counter"
    )

    class Plan(
        val dexPaths: List<String>,
        val nativeLibDir: String,
        /** Libraries the mod's own classes load; loaded afterwards if they did not. */
        val deferredLibraries: List<String>,
        val entryPoints: List<ApkDexScanner.EntryPoint>
    )

    private val loadedLibraries = mutableListOf<String>()
    private val failures = mutableListOf<String>()
    @Volatile
    private var reportPending = false

    /**
     * Loads the extra libraries the APK's activity loads and returns what is left to run in the
     * activity, or null when the APK adds nothing.
     */
    fun prepare(gameManager: GamePackageManager, log: (String) -> Unit): Plan? {
        synchronized(this) {
            loadedLibraries.clear()
            failures.clear()
            reportPending = true
        }
        val apkFiles = gameManager.getApkFiles()
        val dexApks = apkFiles.filter(::containsDex)
        log("APK mod scan: ${apkFiles.size} APK file(s), ${dexApks.size} with code")
        if (dexApks.isEmpty()) return null
        val nativeLibDir = gameManager.getNativeLibraryDir()
        val result = loadCachedScan(dexApks, File(nativeLibDir, CACHE_FILE_NAME))
        if (result == null) {
            recordFailure("APK code could not be read", log)
            return null
        }
        val referenced = result.hostLibraries + result.modClassLibraries
        log("APK mod scan: libraries loaded by the APK's code: ${referenced.joinToString().ifEmpty { "none" }}")

        // Only libraries the APK actually ships; names from SDK code paths it does not use are ignored.
        val extraLibraries = referenced.filter {
            it !in VANILLA_LIBRARIES && !gameManager.isManagedLibrary("lib$it.so")
                && File(nativeLibDir, "lib$it.so").let { file -> file.isFile && file.length() > 0 }
        }
        if (extraLibraries.isEmpty()) log("APK mod scan: no mod libraries in $nativeLibDir")
        // A library that mod classes load is left to them when the launcher will run that code;
        // otherwise, for example when the APK's own Application class loaded it, it is loaded now.
        val (deferred, loadNow) = extraLibraries.partition {
            it in result.modClassLibraries && result.entryPoints.isNotEmpty()
        }
        loadNow.forEach { loadLibrary(gameManager, it, log) }
        if (result.entryPoints.isEmpty() && deferred.isEmpty()) return null
        result.entryPoints.forEach { log("Mod entry point found in APK: $it") }
        return Plan(dexApks.map { it.absolutePath }, nativeLibDir, deferred, result.entryPoints.toList())
    }

    fun recordFailure(message: String, log: (String) -> Unit) {
        synchronized(this) {
            failures.add(message)
            reportPending = true
        }
        log("APK mod: $message")
    }

    /** A one-line result of the last APK mod load for the player, once per launch. */
    fun takeReport(context: Context): String? {
        synchronized(this) {
            if (!reportPending) return null
            reportPending = false
            return when {
                failures.isNotEmpty() -> context.getString(R.string.apk_mod_failed, failures.joinToString("\n"))
                loadedLibraries.isNotEmpty() -> context.getString(R.string.apk_mod_loaded, loadedLibraries.joinToString())
                else -> context.getString(R.string.apk_mod_none)
            }
        }
    }

    private fun loadLibrary(gameManager: GamePackageManager, library: String, log: (String) -> Unit) {
        val loadResult = gameManager.loadLibraryDetailed(library)
        if (loadResult.loaded) {
            synchronized(this) { loadedLibraries.add(loadResult.fileName) }
            log("Loaded mod library from APK: ${loadResult.fileName}")
        } else {
            recordFailure("${loadResult.fileName}: ${loadResult.detail ?: "unknown error"}", log)
        }
    }

    /** Calls the APK's mod entry points with [activity], the way its MainActivity did. */
    fun startEntryPoints(activity: Activity, plan: Plan, gameManager: GamePackageManager, log: (String) -> Unit) {
        val launcherLoader = activity.javaClass.classLoader
        if (plan.entryPoints.isNotEmpty()) {
            val modLoader = try {
                // Android 14+ refuses to load dex code from writable files.
                plan.dexPaths.forEach { File(it).setReadOnly() }
                DexClassLoader(
                    plan.dexPaths.joinToString(File.pathSeparator),
                    activity.codeCacheDir.absolutePath,
                    plan.nativeLibDir,
                    launcherLoader
                )
            } catch (e: Throwable) {
                recordFailure("APK code could not be opened: ${describe(e)}", log)
                null
            }
            if (modLoader != null) {
                plan.entryPoints.forEach { invoke(activity, modLoader, launcherLoader, it, log) }
            }
        }

        for (library in plan.deferredLibraries) {
            val path = File(plan.nativeLibDir, "lib$library.so").absolutePath
            if (isMapped(path)) {
                synchronized(this) { loadedLibraries.add("lib$library.so") }
                continue
            }
            loadLibrary(gameManager, library, log)
        }
    }

    private fun invoke(
        activity: Activity,
        modLoader: ClassLoader,
        launcherLoader: ClassLoader?,
        entry: ApkDexScanner.EntryPoint,
        log: (String) -> Unit
    ) {
        if (isLoadable(launcherLoader, entry.className)) {
            // The launcher ships this class, so it is game code rather than a mod.
            return
        }
        try {
            val owner = Class.forName(entry.className, true, modLoader)
            val method = if (entry.parameterType == null) {
                owner.getDeclaredMethod(entry.methodName)
            } else {
                owner.getDeclaredMethod(entry.methodName, Class.forName(entry.parameterType, false, launcherLoader))
            }
            if (!Modifier.isStatic(method.modifiers)) return
            method.isAccessible = true
            if (entry.parameterType == null) method.invoke(null) else method.invoke(null, activity)
            log("Started mod from APK: $entry")
        } catch (e: Throwable) {
            recordFailure("$entry: ${describe(e)}", log)
        }
    }

    private fun loadCachedScan(apkFiles: List<File>, cacheFile: File): ApkDexScanner.Result? {
        val key = buildString {
            append("version=").append(CACHE_VERSION).append('\n')
            apkFiles.forEach { append("apk=").append(it.absolutePath).append('|').append(it.length())
                .append('|').append(it.lastModified()).append('\n') }
        }
        readCache(cacheFile, key)?.let { return it }

        val result = ApkDexScanner.Result()
        val scanner = ApkDexScanner()
        for (apk in apkFiles) {
            try {
                ZipFile(apk).use { zip ->
                    zip.entries().asSequence().filter { DEX_ENTRY.matches(it.name) }.forEach { entry ->
                        val bytes = zip.getInputStream(entry).use { it.readBytes() }
                        try {
                            scanner.scan(bytes, result)
                        } catch (e: RuntimeException) {
                            Log.w(TAG, "Failed to read ${entry.name} in ${apk.name}: ${e.message}")
                        }
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Failed to open ${apk.absolutePath}: ${e.message}")
                return null
            }
        }
        writeCache(cacheFile, key, result)
        return result
    }

    private fun readCache(cacheFile: File, key: String): ApkDexScanner.Result? {
        val text = try {
            if (!cacheFile.isFile) return null
            cacheFile.readText()
        } catch (_: Exception) {
            return null
        }
        if (!text.startsWith(key)) return null
        val result = ApkDexScanner.Result()
        text.substring(key.length).lineSequence().forEach { line ->
            val value = line.substringAfter('=')
            when (line.substringBefore('=')) {
                "host" -> result.hostLibraries.add(value)
                "mod" -> result.modClassLibraries.add(value)
                "entry" -> {
                    val parts = value.split('|')
                    if (parts.size == 3) {
                        result.entryPoints.add(ApkDexScanner.EntryPoint(parts[0], parts[1], parts[2].ifEmpty { null }))
                    }
                }
            }
        }
        return result
    }

    private fun writeCache(cacheFile: File, key: String, result: ApkDexScanner.Result) {
        try {
            cacheFile.writeText(buildString {
                append(key)
                result.hostLibraries.forEach { append("host=").append(it).append('\n') }
                result.modClassLibraries.forEach { append("mod=").append(it).append('\n') }
                result.entryPoints.forEach {
                    append("entry=").append(it.className).append('|').append(it.methodName).append('|')
                        .append(it.parameterType ?: "").append('\n')
                }
            })
        } catch (e: Exception) {
            Log.w(TAG, "Failed to write ${cacheFile.absolutePath}: ${e.message}")
        }
    }

    private fun containsDex(apk: File): Boolean {
        return try {
            ZipFile(apk).use { it.getEntry("classes.dex") != null }
        } catch (_: Exception) {
            false
        }
    }

    private fun isLoadable(loader: ClassLoader?, className: String): Boolean {
        return try {
            Class.forName(className, false, loader)
            true
        } catch (_: Throwable) {
            false
        }
    }

    private fun isMapped(path: String): Boolean {
        return try {
            File("/proc/self/maps").useLines { lines -> lines.any { it.endsWith(path) } }
        } catch (_: Exception) {
            false
        }
    }

    private fun describe(error: Throwable): String {
        val cause = if (error is InvocationTargetException) error.targetException ?: error else error
        return "${cause.javaClass.simpleName}: ${cause.message ?: ""}".trimEnd(' ', ':')
    }
}
