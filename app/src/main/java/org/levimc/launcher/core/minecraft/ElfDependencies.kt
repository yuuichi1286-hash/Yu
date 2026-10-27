package org.levimc.launcher.core.minecraft

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

/**
 * Reads the DT_NEEDED entries of an ELF shared library from its program headers, the same
 * way the dynamic linker finds them. Returns an empty list for anything it cannot parse.
 */
object ElfDependencies {
    private const val PT_LOAD = 1L
    private const val PT_DYNAMIC = 2L
    private const val DT_NULL = 0L
    private const val DT_NEEDED = 1L
    private const val DT_STRTAB = 5L
    private const val MAX_NAME_LENGTH = 4096

    private class Segment(val type: Long, val offset: Long, val vaddr: Long, val fileSize: Long)

    @JvmStatic
    fun read(file: File): List<String> {
        return try {
            RandomAccessFile(file, "r").use { raf -> read(raf.channel) }
        } catch (_: Exception) {
            emptyList()
        }
    }

    private fun read(channel: FileChannel): List<String> {
        val ident = readAt(channel, 0, 16, ByteOrder.LITTLE_ENDIAN)
        if (ident.getInt(0) != 0x464C457F) return emptyList() // "\u007FELF"
        val is64 = when (ident.get(4).toInt()) {
            1 -> false
            2 -> true
            else -> return emptyList()
        }
        val order = when (ident.get(5).toInt()) {
            1 -> ByteOrder.LITTLE_ENDIAN
            2 -> ByteOrder.BIG_ENDIAN
            else -> return emptyList()
        }

        val header = readAt(channel, 0, if (is64) 64 else 52, order)
        val phOff = if (is64) header.getLong(0x20) else header.getInt(0x1C).toUInt().toLong()
        val phEntSize = header.getShort(if (is64) 0x36 else 0x2A).toUShort().toInt()
        val phNum = header.getShort(if (is64) 0x38 else 0x2C).toUShort().toInt()
        if (phOff <= 0 || phEntSize <= 0 || phNum <= 0) return emptyList()

        val segments = (0 until phNum).map { index ->
            val ph = readAt(channel, phOff + index.toLong() * phEntSize, if (is64) 56 else 32, order)
            if (is64) {
                Segment(ph.getInt(0).toUInt().toLong(), ph.getLong(8), ph.getLong(16), ph.getLong(32))
            } else {
                Segment(
                    ph.getInt(0).toUInt().toLong(),
                    ph.getInt(4).toUInt().toLong(),
                    ph.getInt(8).toUInt().toLong(),
                    ph.getInt(16).toUInt().toLong()
                )
            }
        }
        val dynamic = segments.firstOrNull { it.type == PT_DYNAMIC } ?: return emptyList()

        val entrySize = if (is64) 16 else 8
        val entryCount = (dynamic.fileSize / entrySize).toInt()
        val table = readAt(channel, dynamic.offset, entryCount * entrySize, order)
        val neededOffsets = mutableListOf<Long>()
        var strTabAddress = -1L
        for (i in 0 until entryCount) {
            val tag = if (is64) table.getLong(i * entrySize) else table.getInt(i * entrySize).toLong()
            val value = if (is64) {
                table.getLong(i * entrySize + 8)
            } else {
                table.getInt(i * entrySize + 4).toUInt().toLong()
            }
            when (tag) {
                DT_NULL -> break
                DT_NEEDED -> neededOffsets.add(value)
                DT_STRTAB -> strTabAddress = value
            }
        }
        if (neededOffsets.isEmpty() || strTabAddress < 0) return emptyList()

        val strTab = segments.firstOrNull {
            it.type == PT_LOAD && strTabAddress >= it.vaddr && strTabAddress < it.vaddr + it.fileSize
        } ?: return emptyList()
        val strTabOffset = strTabAddress - strTab.vaddr + strTab.offset
        return neededOffsets.mapNotNull { readString(channel, strTabOffset + it) }
    }

    private fun readAt(channel: FileChannel, position: Long, size: Int, order: ByteOrder): ByteBuffer {
        val buffer = ByteBuffer.allocate(size).order(order)
        var read = 0
        while (read < size) {
            val count = channel.read(buffer, position + read)
            if (count <= 0) throw IllegalArgumentException("Truncated ELF file")
            read += count
        }
        buffer.flip()
        return buffer
    }

    private fun readString(channel: FileChannel, position: Long): String? {
        val available = (channel.size() - position).coerceAtMost(MAX_NAME_LENGTH.toLong()).toInt()
        if (available <= 0) return null
        val bytes = readAt(channel, position, available, ByteOrder.LITTLE_ENDIAN)
        val end = (0 until available).firstOrNull { bytes.get(it).toInt() == 0 } ?: return null
        if (end == 0) return null
        return String(bytes.array(), 0, end, Charsets.UTF_8)
    }
}
