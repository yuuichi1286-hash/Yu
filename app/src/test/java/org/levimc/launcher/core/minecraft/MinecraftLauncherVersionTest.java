package org.levimc.launcher.core.minecraft;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class MinecraftLauncherVersionTest {
    @Test
    public void versionsBefore12180AreFlaggedAsUntested() {
        assertTrue(MinecraftLauncher.isOlderThanTestedVersion("1.21.70"));
        assertTrue(MinecraftLauncher.isOlderThanTestedVersion("1.20.81.01"));
        assertTrue(MinecraftLauncher.isOlderThanTestedVersion("1.16.201"));
    }

    @Test
    public void testedAndUnknownVersionsAreNotFlagged() {
        assertFalse(MinecraftLauncher.isOlderThanTestedVersion("1.21.80"));
        assertFalse(MinecraftLauncher.isOlderThanTestedVersion("1.21.100.06-vulkan"));
        assertFalse(MinecraftLauncher.isOlderThanTestedVersion("1.26.70.20"));
        assertFalse(MinecraftLauncher.isOlderThanTestedVersion("custom"));
        assertFalse(MinecraftLauncher.isOlderThanTestedVersion(null));
    }
}
