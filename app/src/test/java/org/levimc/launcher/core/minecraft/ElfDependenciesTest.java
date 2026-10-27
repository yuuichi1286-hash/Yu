package org.levimc.launcher.core.minecraft;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public class ElfDependenciesTest {
    @Rule
    public TemporaryFolder temporaryFolder = new TemporaryFolder();

    @Test
    public void readsNeededLibrariesFromProgramHeaders() throws Exception {
        File elf = temporaryFolder.newFile("libminecraftpe.so");
        write(elf, buildElf64("libc++_shared.so", "librenderer.so"));

        assertEquals(Arrays.asList("libc++_shared.so", "librenderer.so"), ElfDependencies.read(elf));
    }

    @Test
    public void returnsEmptyListForNonElfFiles() throws Exception {
        File notElf = temporaryFolder.newFile("libbroken.so");
        write(notElf, "not an elf file".getBytes(StandardCharsets.UTF_8));

        assertTrue(ElfDependencies.read(notElf).isEmpty());
        assertTrue(ElfDependencies.read(new File(temporaryFolder.getRoot(), "missing.so")).isEmpty());
    }

    private static byte[] buildElf64(String... needed) {
        final long baseAddress = 0x10000;
        final int strTabOffset = 176;
        final int dynamicOffset = 256;

        ByteBuffer strTab = ByteBuffer.allocate(64);
        strTab.put((byte) 0);
        long[] nameOffsets = new long[needed.length];
        for (int i = 0; i < needed.length; i++) {
            nameOffsets[i] = strTab.position();
            strTab.put(needed[i].getBytes(StandardCharsets.UTF_8)).put((byte) 0);
        }

        int dynamicSize = (needed.length + 2) * 16;
        int total = dynamicOffset + dynamicSize;
        ByteBuffer elf = ByteBuffer.allocate(total).order(ByteOrder.LITTLE_ENDIAN);
        elf.put(new byte[] {0x7F, 'E', 'L', 'F', 2, 1, 1});
        elf.putShort(0x10, (short) 3);
        elf.putShort(0x12, (short) 0xB7);
        elf.putLong(0x20, 64);
        elf.putShort(0x34, (short) 64);
        elf.putShort(0x36, (short) 56);
        elf.putShort(0x38, (short) 2);

        // PT_LOAD mapping the whole file at a non-zero address.
        elf.putInt(64, 1);
        elf.putLong(64 + 8, 0);
        elf.putLong(64 + 16, baseAddress);
        elf.putLong(64 + 32, total);
        elf.putLong(64 + 40, total);
        // PT_DYNAMIC
        elf.putInt(120, 2);
        elf.putLong(120 + 8, dynamicOffset);
        elf.putLong(120 + 16, baseAddress + dynamicOffset);
        elf.putLong(120 + 32, dynamicSize);
        elf.putLong(120 + 40, dynamicSize);

        elf.position(strTabOffset);
        elf.put(strTab.array(), 0, strTab.position());

        elf.position(dynamicOffset);
        for (long nameOffset : nameOffsets) {
            elf.putLong(1).putLong(nameOffset);
        }
        elf.putLong(5).putLong(baseAddress + strTabOffset);
        elf.putLong(0).putLong(0);
        return elf.array();
    }

    private static void write(File file, byte[] bytes) throws Exception {
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(bytes);
        }
    }
}
