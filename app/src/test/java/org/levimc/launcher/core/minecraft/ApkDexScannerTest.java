package org.levimc.launcher.core.minecraft;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;

public class ApkDexScannerTest {
    private static final String MAIN_ACTIVITY = "Lcom/mojang/minecraftpe/MainActivity;";

    @Test
    public void findsLibrariesAndEntryPointsInjectedIntoMainActivity() {
        DexBuilder dex = new DexBuilder();
        int loadLibrary = dex.method("Ljava/lang/System;", "loadLibrary", "V", "Ljava/lang/String;");
        int start = dex.method("Lcom/android/support/Main;", "Start", "V", "Landroid/content/Context;");
        int init = dex.method("Lcom/shader/Loader;", "init", "V");
        int firebase = dex.method("Lcom/google/firebase/FirebaseApp;", "initializeApp", "V", "Landroid/content/Context;");
        int twoArgs = dex.method("Lcom/shader/Loader;", "configure", "V", "Landroid/content/Context;", "I");
        int onCreate = dex.method(MAIN_ACTIVITY, "onCreate", "V", "Landroid/os/Bundle;");
        int clinit = dex.method("Lcom/android/support/Main;", "<clinit>", "V");

        dex.classDef(MAIN_ACTIVITY, "Landroid/app/Activity;", onCreate, concat(
                constWide(0),                              // 5-unit instruction before the injected code
                constString(1, dex.string("minecraftpe")),
                invokeStatic(loadLibrary, 1),
                constString(2, dex.string("shadermenu")),
                invokeStaticRange(loadLibrary, 2, 1),
                invokeStatic(start, 3),
                invokeStatic(init, -1),
                invokeStatic(firebase, 3),
                invokeStatic(twoArgs, 3, 4),
                new int[] {0x000E},                        // return-void
                new int[] {0x0100, 1, 0, 0, 0, 0}          // packed-switch payload
        ));
        dex.classDef("Lcom/android/support/Main;", "Ljava/lang/Object;", clinit, concat(
                constString(0, dex.string("modmenu")),
                invokeStatic(loadLibrary, 0),
                new int[] {0x000E}
        ));

        ApkDexScanner.Result result = new ApkDexScanner.Result();
        new ApkDexScanner().scan(dex.build(), result);

        assertEquals(new LinkedHashSet<>(Arrays.asList("minecraftpe", "shadermenu")), result.hostLibraries);
        assertEquals(Collections.singleton("modmenu"), result.modClassLibraries);
        assertEquals(new LinkedHashSet<>(Arrays.asList(
                new ApkDexScanner.EntryPoint("com.android.support.Main", "Start", "android.content.Context"),
                new ApkDexScanner.EntryPoint("com.shader.Loader", "init", null)
        )), result.entryPoints);
    }

    @Test
    public void vanillaActivityReportsNothingToRun() {
        DexBuilder dex = new DexBuilder();
        int loadLibrary = dex.method("Ljava/lang/System;", "loadLibrary", "V", "Ljava/lang/String;");
        int helper = dex.method("Lcom/mojang/minecraftpe/Helper;", "init", "V", "Landroid/content/Context;");
        int minified = dex.method("La/b/c;", "a", "V", "Landroid/content/Context;");
        int onCreate = dex.method(MAIN_ACTIVITY, "onCreate", "V", "Landroid/os/Bundle;");
        dex.classDef(MAIN_ACTIVITY, "Landroid/app/Activity;", onCreate, concat(
                constString(0, dex.string("fmod")),
                invokeStatic(loadLibrary, 0),
                invokeStatic(helper, 1),
                invokeStatic(minified, 1),
                new int[] {0x000E}
        ));

        ApkDexScanner.Result result = new ApkDexScanner.Result();
        new ApkDexScanner().scan(dex.build(), result);

        assertEquals(Collections.singleton("fmod"), result.hostLibraries);
        assertTrue(result.entryPoints.isEmpty());
        assertTrue(result.modClassLibraries.isEmpty());
    }

    @Test(expected = IllegalArgumentException.class)
    public void rejectsNonDexInput() {
        new ApkDexScanner().scan("PK not a dex file at all, just some bytes......................................................................................".getBytes(StandardCharsets.UTF_8), new ApkDexScanner.Result());
    }

    private static int[] constString(int register, int stringIdx) {
        return new int[] {0x1A | (register << 8), stringIdx};
    }

    private static int[] constWide(int register) {
        return new int[] {0x18 | (register << 8), 1, 2, 3, 4};
    }

    private static int[] invokeStatic(int methodIdx, int... registers) {
        if (registers.length == 1 && registers[0] < 0) registers = new int[0];
        int packed = 0;
        for (int i = 0; i < registers.length; i++) packed |= registers[i] << (4 * i);
        return new int[] {0x71 | (registers.length << 12), methodIdx, packed & 0xFFFF};
    }

    private static int[] invokeStaticRange(int methodIdx, int firstRegister, int count) {
        return new int[] {0x77 | (count << 8), methodIdx, firstRegister};
    }

    private static int[] concat(int[]... parts) {
        int length = 0;
        for (int[] part : parts) length += part.length;
        int[] out = new int[length];
        int pos = 0;
        for (int[] part : parts) {
            System.arraycopy(part, 0, out, pos, part.length);
            pos += part.length;
        }
        return out;
    }

    /** Writes the parts of a dex file that {@link ApkDexScanner} reads. */
    private static final class DexBuilder {
        private final List<String> strings = new ArrayList<>();
        private final List<Integer> types = new ArrayList<>();
        private final List<int[]> protos = new ArrayList<>();      // {return type, parameter types...}
        private final List<int[]> methods = new ArrayList<>();     // {class type, proto, name string}
        private final List<int[]> classDefs = new ArrayList<>();   // {class type, super type, method}
        private final List<int[]> codes = new ArrayList<>();

        int string(String value) {
            int index = strings.indexOf(value);
            if (index >= 0) return index;
            strings.add(value);
            return strings.size() - 1;
        }

        int type(String descriptor) {
            int stringIdx = string(descriptor);
            int index = types.indexOf(stringIdx);
            if (index >= 0) return index;
            types.add(stringIdx);
            return types.size() - 1;
        }

        int method(String owner, String name, String returnType, String... parameters) {
            int[] proto = new int[parameters.length + 1];
            proto[0] = type(returnType);
            for (int i = 0; i < parameters.length; i++) proto[i + 1] = type(parameters[i]);
            protos.add(proto);
            methods.add(new int[] {type(owner), protos.size() - 1, string(name)});
            return methods.size() - 1;
        }

        void classDef(String descriptor, String superclass, int methodIdx, int[] code) {
            classDefs.add(new int[] {type(descriptor), type(superclass), methodIdx});
            codes.add(code);
        }

        byte[] build() {
            int stringIdsOff = 0x70;
            int typeIdsOff = stringIdsOff + strings.size() * 4;
            int protoIdsOff = typeIdsOff + types.size() * 4;
            int methodIdsOff = protoIdsOff + protos.size() * 12;
            int classDefsOff = methodIdsOff + methods.size() * 8;
            int dataOff = classDefsOff + classDefs.size() * 32;

            ByteArrayOutputStream data = new ByteArrayOutputStream();
            int[] stringDataOffs = new int[strings.size()];
            for (int i = 0; i < strings.size(); i++) {
                stringDataOffs[i] = dataOff + data.size();
                byte[] bytes = strings.get(i).getBytes(StandardCharsets.UTF_8);
                writeUleb(data, bytes.length);
                data.write(bytes, 0, bytes.length);
                data.write(0);
            }
            int[] parameterOffs = new int[protos.size()];
            for (int i = 0; i < protos.size(); i++) {
                int[] proto = protos.get(i);
                if (proto.length == 1) continue;
                align(data, 4);
                parameterOffs[i] = dataOff + data.size();
                writeInt(data, proto.length - 1);
                for (int p = 1; p < proto.length; p++) writeShort(data, proto[p]);
            }
            int[] codeOffs = new int[codes.size()];
            for (int i = 0; i < codes.size(); i++) {
                align(data, 4);
                codeOffs[i] = dataOff + data.size();
                int[] code = codes.get(i);
                writeShort(data, 8);
                writeShort(data, 1);
                writeShort(data, 5);
                writeShort(data, 0);
                writeInt(data, 0);
                writeInt(data, code.length);
                for (int unit : code) writeShort(data, unit);
            }
            int[] classDataOffs = new int[classDefs.size()];
            for (int i = 0; i < classDefs.size(); i++) {
                classDataOffs[i] = dataOff + data.size();
                writeUleb(data, 1);  // one static field
                writeUleb(data, 0);
                writeUleb(data, 1);  // one direct method
                writeUleb(data, 0);
                writeUleb(data, 0);  // field_idx_diff
                writeUleb(data, 8);  // access flags
                writeUleb(data, classDefs.get(i)[2]);
                writeUleb(data, 0x10009);
                writeUleb(data, codeOffs[i]);
            }

            ByteBuffer out = ByteBuffer.allocate(dataOff + data.size()).order(ByteOrder.LITTLE_ENDIAN);
            out.put("dex\n035\0".getBytes(StandardCharsets.US_ASCII));
            out.putInt(0x20, out.capacity());
            out.putInt(0x24, 0x70);
            out.putInt(0x28, 0x12345678);
            out.putInt(0x38, strings.size()).putInt(0x3C, stringIdsOff);
            out.putInt(0x40, types.size()).putInt(0x44, typeIdsOff);
            out.putInt(0x48, protos.size()).putInt(0x4C, protoIdsOff);
            out.putInt(0x58, methods.size()).putInt(0x5C, methodIdsOff);
            out.putInt(0x60, classDefs.size()).putInt(0x64, classDefsOff);
            for (int i = 0; i < strings.size(); i++) out.putInt(stringIdsOff + i * 4, stringDataOffs[i]);
            for (int i = 0; i < types.size(); i++) out.putInt(typeIdsOff + i * 4, types.get(i));
            for (int i = 0; i < protos.size(); i++) {
                int base = protoIdsOff + i * 12;
                out.putInt(base, string("V"));
                out.putInt(base + 4, protos.get(i)[0]);
                out.putInt(base + 8, parameterOffs[i]);
            }
            for (int i = 0; i < methods.size(); i++) {
                int[] method = methods.get(i);
                int base = methodIdsOff + i * 8;
                out.putShort(base, (short) method[0]);
                out.putShort(base + 2, (short) method[1]);
                out.putInt(base + 4, method[2]);
            }
            for (int i = 0; i < classDefs.size(); i++) {
                int base = classDefsOff + i * 32;
                out.putInt(base, classDefs.get(i)[0]);
                out.putInt(base + 8, classDefs.get(i)[1]);
                out.putInt(base + 12, 0);
                out.putInt(base + 16, -1);
                out.putInt(base + 24, classDataOffs[i]);
            }
            out.position(dataOff);
            out.put(data.toByteArray());
            return out.array();
        }

        private static void align(ByteArrayOutputStream data, int alignment) {
            while (data.size() % alignment != 0) data.write(0);
        }

        private static void writeUleb(ByteArrayOutputStream out, int value) {
            do {
                int b = value & 0x7F;
                value >>>= 7;
                out.write(value != 0 ? b | 0x80 : b);
            } while (value != 0);
        }

        private static void writeShort(ByteArrayOutputStream out, int value) {
            out.write(value & 0xFF);
            out.write((value >>> 8) & 0xFF);
        }

        private static void writeInt(ByteArrayOutputStream out, int value) {
            writeShort(out, value);
            writeShort(out, value >>> 16);
        }
    }
}
