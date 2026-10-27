package org.levimc.launcher.core.minecraft;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/**
 * Finds what a modified Minecraft APK adds to Mojang's activity classes.
 *
 * The launcher runs its own copy of {@code com.mojang.minecraftpe.MainActivity}, so code that a
 * mod injects into the APK's MainActivity (usually {@code System.loadLibrary("menu")} or a static
 * call such as {@code Menu.Start(this)}) never runs. This reads the APK's dex files and reports
 * those library loads and static calls so the launcher can repeat them.
 */
public final class ApkDexScanner {
    private static final int NO_INDEX = -1;

    private static final String SYSTEM_CLASS = "Ljava/lang/System;";
    private static final String APPLICATION_CLASS = "Landroid/app/Application;";
    private static final String MINECRAFT_PACKAGE = "Lcom/mojang/minecraftpe/";

    private static final Set<String> DEFAULT_HOST_CLASSES = new HashSet<>(Arrays.asList(
            "Lcom/mojang/minecraftpe/MainActivity;",
            "Lcom/mojang/minecraftpe/Launcher;"
    ));

    /** Parameter types an entry point may take; the launcher passes the game activity. */
    private static final Set<String> ENTRY_PARAMETER_TYPES = new HashSet<>(Arrays.asList(
            "Landroid/content/Context;",
            "Landroid/content/ContextWrapper;",
            "Landroid/view/ContextThemeWrapper;",
            "Landroid/app/Activity;",
            "Lcom/mojang/minecraftpe/MainActivity;",
            "Lcom/mojang/minecraftpe/Launcher;"
    ));

    /** Code from the game itself or from SDKs it ships, never treated as mod code. */
    private static final String[] VENDOR_PREFIXES = {
            "Landroid/", "Landroidx/", "Ljava/", "Ljavax/", "Ldalvik/", "Lsun/", "Llibcore/",
            "Lkotlin/", "Lkotlinx/", "Lorg/json/", "Lorg/apache/", "Lorg/xmlpull/", "Lorg/w3c/",
            "Lcom/mojang/", "Lcom/microsoft/", "Lcom/xbox/", "Lorg/fmod/", "Lcom/google/",
            "Lcom/android/billingclient/", "Lcom/android/vending/", "Lcom/android/installreferrer/",
            "Lcom/amazon/", "Lcom/pairip/", "Lcom/appsflyer/", "Lcom/facebook/", "Lcom/samsung/",
            "Lcom/oculus/", "Lcom/meta/", "Lokhttp3/", "Lokio/", "Lorg/conscrypt/"
    };

    public static final class EntryPoint {
        /** Binary class name, for example {@code com.android.support.Main}. */
        public final String className;
        public final String methodName;
        /** Binary name of the single parameter type, or {@code null} for a method without one. */
        public final String parameterType;

        public EntryPoint(String className, String methodName, String parameterType) {
            this.className = className;
            this.methodName = methodName;
            this.parameterType = parameterType;
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof EntryPoint)) return false;
            EntryPoint other = (EntryPoint) o;
            return className.equals(other.className)
                    && methodName.equals(other.methodName)
                    && Objects.equals(parameterType, other.parameterType);
        }

        @Override
        public int hashCode() {
            return Objects.hash(className, methodName, parameterType);
        }

        @Override
        public String toString() {
            return className + "." + methodName + "(" + (parameterType == null ? "" : parameterType) + ")";
        }
    }

    public static final class Result {
        /** Libraries loaded by Mojang's activity or application classes. */
        public final Set<String> hostLibraries = new LinkedHashSet<>();
        /** Libraries loaded by mod classes themselves; their own code loads them. */
        public final Set<String> modClassLibraries = new LinkedHashSet<>();
        /** Static methods outside the game that Mojang's activity or application classes call. */
        public final Set<EntryPoint> entryPoints = new LinkedHashSet<>();

        public boolean isEmpty() {
            return hostLibraries.isEmpty() && modClassLibraries.isEmpty() && entryPoints.isEmpty();
        }
    }

    private final Set<String> hostClasses;

    public ApkDexScanner() {
        this(DEFAULT_HOST_CLASSES);
    }

    ApkDexScanner(Set<String> hostClasses) {
        this.hostClasses = hostClasses;
    }

    /** Adds what {@code dex} contains to {@code result}. Throws on malformed input. */
    public void scan(byte[] dex, Result result) {
        new DexFile(dex).scan(result);
    }

    static boolean isVendorType(String descriptor) {
        for (String prefix : VENDOR_PREFIXES) {
            if (descriptor.startsWith(prefix)) return true;
        }
        return false;
    }

    static boolean isObfuscatedName(String descriptor) {
        for (String segment : descriptor.substring(1, descriptor.length() - 1).split("/")) {
            if (segment.length() > 2) return false;
        }
        return true;
    }

    static String toBinaryName(String descriptor) {
        return descriptor.substring(1, descriptor.length() - 1).replace('/', '.');
    }

    private final class DexFile {
        private final ByteBuffer buffer;
        private final int stringIdsSize;
        private final int stringIdsOff;
        private final int typeIdsSize;
        private final int typeIdsOff;
        private final int protoIdsSize;
        private final int protoIdsOff;
        private final int methodIdsSize;
        private final int methodIdsOff;
        private final int classDefsSize;
        private final int classDefsOff;
        private final Map<Integer, String> stringCache = new HashMap<>();

        DexFile(byte[] dex) {
            if (dex.length < 0x70 || dex[0] != 'd' || dex[1] != 'e' || dex[2] != 'x' || dex[3] != '\n') {
                throw new IllegalArgumentException("Not a dex file");
            }
            buffer = ByteBuffer.wrap(dex).order(ByteOrder.LITTLE_ENDIAN);
            stringIdsSize = buffer.getInt(0x38);
            stringIdsOff = buffer.getInt(0x3C);
            typeIdsSize = buffer.getInt(0x40);
            typeIdsOff = buffer.getInt(0x44);
            protoIdsSize = buffer.getInt(0x48);
            protoIdsOff = buffer.getInt(0x4C);
            methodIdsSize = buffer.getInt(0x58);
            methodIdsOff = buffer.getInt(0x5C);
            classDefsSize = buffer.getInt(0x60);
            classDefsOff = buffer.getInt(0x64);
        }

        void scan(Result result) {
            for (int i = 0; i < classDefsSize; i++) {
                int def = classDefsOff + i * 32;
                String descriptor = typeName(buffer.getInt(def));
                int superIdx = buffer.getInt(def + 8);
                int classDataOff = buffer.getInt(def + 24);
                if (classDataOff == 0) continue;

                boolean host = hostClasses.contains(descriptor)
                        || (descriptor.startsWith(MINECRAFT_PACKAGE)
                        && superIdx != NO_INDEX && APPLICATION_CLASS.equals(typeName(superIdx)));
                if (host) {
                    scanClass(classDataOff, result.hostLibraries, result.entryPoints);
                } else if (!isVendorType(descriptor)) {
                    scanClass(classDataOff, result.modClassLibraries, null);
                }
            }
        }

        private void scanClass(int classDataOff, Set<String> libraries, Set<EntryPoint> entryPoints) {
            int[] pos = {classDataOff};
            int staticFields = readUleb(pos);
            int instanceFields = readUleb(pos);
            int directMethods = readUleb(pos);
            int virtualMethods = readUleb(pos);
            for (int i = 0; i < (staticFields + instanceFields) * 2; i++) readUleb(pos);
            for (int group = 0; group < 2; group++) {
                int count = group == 0 ? directMethods : virtualMethods;
                for (int i = 0; i < count; i++) {
                    readUleb(pos); // method_idx_diff
                    readUleb(pos); // access_flags
                    int codeOff = readUleb(pos);
                    if (codeOff != 0) scanCode(codeOff, libraries, entryPoints);
                }
            }
        }

        private void scanCode(int codeOff, Set<String> libraries, Set<EntryPoint> entryPoints) {
            int insnsSize = buffer.getInt(codeOff + 12);
            int start = codeOff + 16;
            int end = start + insnsSize * 2;
            if (insnsSize < 0 || end > buffer.capacity() || end < start) {
                throw new IllegalArgumentException("Bad code item");
            }
            Map<Integer, String> registerStrings = new HashMap<>();
            int pc = 0;
            while (pc < insnsSize) {
                int unit = unit(start, pc);
                int opcode = unit & 0xFF;
                switch (opcode) {
                    case 0x1A: // const-string vAA, string@BBBB
                        registerStrings.put(unit >>> 8, string(unit(start, pc + 1)));
                        break;
                    case 0x1B: // const-string/jumbo vAA, string@BBBBBBBB
                        registerStrings.put(unit >>> 8, string(unit(start, pc + 1) | (unit(start, pc + 2) << 16)));
                        break;
                    case 0x71: // invoke-static {vC, vD, ...}, meth@BBBB
                        handleInvoke(unit(start, pc + 1), (unit >>> 12) & 0xF, unit(start, pc + 2) & 0xF,
                                registerStrings, libraries, entryPoints);
                        break;
                    case 0x77: // invoke-static/range {vCCCC .. vNNNN}, meth@BBBB
                        handleInvoke(unit(start, pc + 1), unit >>> 8, unit(start, pc + 2),
                                registerStrings, libraries, entryPoints);
                        break;
                    default:
                        break;
                }
                pc += instructionWidth(start, pc, insnsSize);
            }
        }

        private void handleInvoke(int methodIdx, int argCount, int firstRegister, Map<Integer, String> registerStrings,
                                  Set<String> libraries, Set<EntryPoint> entryPoints) {
            if (methodIdx < 0 || methodIdx >= methodIdsSize) return;
            int method = methodIdsOff + methodIdx * 8;
            String owner = typeName(buffer.getShort(method) & 0xFFFF);
            String name = string(buffer.getInt(method + 4));
            String[] parameters = protoParameters(buffer.getShort(method + 2) & 0xFFFF);

            if (SYSTEM_CLASS.equals(owner) && "loadLibrary".equals(name)
                    && parameters.length == 1 && argCount == 1) {
                String library = registerStrings.get(firstRegister);
                if (library != null && !library.isEmpty()) libraries.add(library);
                return;
            }
            if (entryPoints == null || isVendorType(owner) || !owner.startsWith("L")) return;
            // Minified game code (a.b.c) is not a mod; running it a second time could break the game.
            if (isObfuscatedName(owner)) return;
            if (name.startsWith("<") || name.contains("$")) return;
            if (parameters.length == 0) {
                entryPoints.add(new EntryPoint(toBinaryName(owner), name, null));
            } else if (parameters.length == 1 && ENTRY_PARAMETER_TYPES.contains(parameters[0])) {
                entryPoints.add(new EntryPoint(toBinaryName(owner), name, toBinaryName(parameters[0])));
            }
        }

        private int instructionWidth(int start, int pc, int insnsSize) {
            int unit = unit(start, pc);
            int opcode = unit & 0xFF;
            if (opcode == 0x00 && unit != 0) {
                // Payload pseudo-instructions follow the method's real code.
                switch (unit) {
                    case 0x0100: return unit(start, pc + 1) * 2 + 4;
                    case 0x0200: return unit(start, pc + 1) * 4 + 2;
                    case 0x0300: {
                        int elementWidth = unit(start, pc + 1);
                        long count = (unit(start, pc + 2) & 0xFFFFL) | ((long) unit(start, pc + 3) << 16);
                        long width = (count * elementWidth + 1) / 2 + 4;
                        return (int) Math.min(width, insnsSize - pc);
                    }
                    default: return 1;
                }
            }
            return WIDTHS[opcode];
        }

        private int unit(int start, int index) {
            return buffer.getShort(start + index * 2) & 0xFFFF;
        }

        private String typeName(int typeIdx) {
            if (typeIdx < 0 || typeIdx >= typeIdsSize) throw new IllegalArgumentException("Bad type index");
            return string(buffer.getInt(typeIdsOff + typeIdx * 4));
        }

        private String[] protoParameters(int protoIdx) {
            if (protoIdx >= protoIdsSize) throw new IllegalArgumentException("Bad proto index");
            int parametersOff = buffer.getInt(protoIdsOff + protoIdx * 12 + 8);
            if (parametersOff == 0) return new String[0];
            int size = buffer.getInt(parametersOff);
            if (size < 0 || size > 255) throw new IllegalArgumentException("Bad type list");
            String[] parameters = new String[size];
            for (int i = 0; i < size; i++) {
                parameters[i] = typeName(buffer.getShort(parametersOff + 4 + i * 2) & 0xFFFF);
            }
            return parameters;
        }

        private String string(int stringIdx) {
            if (stringIdx < 0 || stringIdx >= stringIdsSize) throw new IllegalArgumentException("Bad string index");
            String cached = stringCache.get(stringIdx);
            if (cached != null) return cached;
            int[] pos = {buffer.getInt(stringIdsOff + stringIdx * 4)};
            int length = readUleb(pos);
            StringBuilder sb = new StringBuilder(length);
            int p = pos[0];
            for (int i = 0; i < length; i++) {
                int a = buffer.get(p++) & 0xFF;
                if (a < 0x80) {
                    sb.append((char) a);
                } else if ((a & 0xE0) == 0xC0) {
                    int b = buffer.get(p++) & 0x3F;
                    sb.append((char) (((a & 0x1F) << 6) | b));
                } else {
                    int b = buffer.get(p++) & 0x3F;
                    int c = buffer.get(p++) & 0x3F;
                    sb.append((char) (((a & 0x0F) << 12) | (b << 6) | c));
                }
            }
            String value = sb.toString();
            stringCache.put(stringIdx, value);
            return value;
        }

        private int readUleb(int[] pos) {
            int result = 0;
            for (int shift = 0; shift < 35; shift += 7) {
                int b = buffer.get(pos[0]++) & 0xFF;
                result |= (b & 0x7F) << shift;
                if ((b & 0x80) == 0) return result;
            }
            throw new IllegalArgumentException("Bad uleb128");
        }
    }

    /** Instruction widths in 16-bit code units, indexed by opcode. */
    private static final int[] WIDTHS = new int[256];

    static {
        Arrays.fill(WIDTHS, 1);
        int[][] ranges = {
                // {first opcode, last opcode, width}
                {0x02, 0x02, 2}, {0x03, 0x03, 3}, {0x05, 0x05, 2}, {0x06, 0x06, 3},
                {0x08, 0x08, 2}, {0x09, 0x09, 3}, {0x13, 0x13, 2}, {0x14, 0x14, 3},
                {0x15, 0x16, 2}, {0x17, 0x17, 3}, {0x18, 0x18, 5}, {0x19, 0x1A, 2},
                {0x1B, 0x1B, 3}, {0x1C, 0x1C, 2}, {0x1F, 0x20, 2}, {0x22, 0x23, 2},
                {0x24, 0x26, 3}, {0x29, 0x29, 2}, {0x2A, 0x2C, 3}, {0x2D, 0x3D, 2},
                {0x44, 0x6D, 2}, {0x6E, 0x72, 3}, {0x74, 0x78, 3}, {0x90, 0xAF, 2},
                {0xD0, 0xE2, 2}, {0xFA, 0xFB, 4}, {0xFC, 0xFD, 3}, {0xFE, 0xFF, 2},
        };
        for (int[] range : ranges) {
            for (int op = range[0]; op <= range[1]; op++) WIDTHS[op] = range[2];
        }
    }
}
