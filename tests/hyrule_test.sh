#!/usr/libexec/atf-sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Mark LaPointe <mark@cloudbsd.org>

atf_test_case existence cleanup
existence_head() {
	atf_set "descr" "Verify that Hyrule device files are created"
	atf_set "require.user" "root"
}
existence_body() {
	# Ensure the module is loaded (relative to the test source dir)
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	
	atf_check test -c /dev/hyrule/help
	atf_check test -c /dev/hyrule/characters/link/stats/health
	atf_check test -c /dev/hyrule/characters/zelda/stats/health
	atf_check test -c /dev/hyrule/characters/ganon/stats/health
	atf_check test -c /dev/hyrule/objects/triforce/parts/courage
}
existence_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case help_device cleanup
help_device_head() {
	atf_set "descr" "Verify help device content"
	atf_set "require.user" "root"
}
help_device_body() {
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	atf_check -o "match:Welcome to the Hyrule Kernel Module!" cat /dev/hyrule/help
	atf_check -o "match:Be careful, it's dangerous to go alone!" cat /dev/hyrule/help
}
help_device_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case stats_check cleanup
stats_check_head() {
	atf_set "descr" "Verify character stats"
	atf_set "require.user" "root"
}
stats_check_body() {
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	atf_check -o "inline:100\n" cat /dev/hyrule/characters/link/stats/health
	atf_check -o "inline:100\n" cat /dev/hyrule/characters/zelda/stats/health
	atf_check -o "inline:200\n" cat /dev/hyrule/characters/ganon/stats/health
	atf_check -o "inline:ALIVE\n" cat /dev/hyrule/characters/ganon/status/condition
}
stats_check_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case man_page cleanup
man_page_head() {
	atf_set "descr" "Verify manual page existence"
}
man_page_body() {
	# Check if the man page can be found locally
	atf_check -o "match:Hyrulian Kernel Interface" man -l $(atf_get_srcdir)/../hyrule.4
}
man_page_cleanup() {
	# Nothing to cleanup
	:
}

atf_test_case write_read cleanup
write_read_head() {
	atf_set "descr" "Verify writing and reading back"
	atf_set "require.user" "root"
}
write_read_body() {
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	echo "Hylian Shield" > /dev/hyrule/characters/link/weapons/sword
	atf_check -o "inline:Hylian Shield\n" cat /dev/hyrule/characters/link/weapons/sword
}
write_read_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case offset_test cleanup
offset_test_head() {
	atf_set "descr" "Verify offset handling in read and write"
	atf_set "require.user" "root"
}
offset_test_body() {
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	
	# Read with offset 7: "Master Sword\n" -> "Sword\n"
	# We use tail -c +8 which starts at the 8th byte (offset 7)
	atf_check -o "inline:Sword\n" sh -c "cat /dev/hyrule/characters/link/weapons/sword | tail -c +8"
	
	# Write with offset: "Master Sword\n"
	# At offset 12: overwrite "\n" with " of Power\n"
	# Using perl for reliable seeking on char device
	atf_check perl -e 'open(my $f, "+>", "/dev/hyrule/characters/link/weapons/sword") or die $!; seek($f, 12, 0); print $f " of Power\n";'
	atf_check -o "inline:Master Sword of Power\n" cat /dev/hyrule/characters/link/weapons/sword
}
offset_test_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case buffer_limit cleanup
buffer_limit_head() {
	atf_set "descr" "Verify buffer limits"
	atf_set "require.user" "root"
}
buffer_limit_body() {
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	
	# Try to write at offset 512, which should return an error.
	# We accept multiple error messages because dd/perl/kernel might report differently.
	# Operation not supported by device (ENODEV) is common for seek failures on some char devices.
	# File too large (EFBIG) is what we return in our code.
	atf_check -s not-exit:0 -e "match:File too large|Operation not supported" \
		sh -c "printf '!' | dd of=/dev/hyrule/characters/link/weapons/sword bs=1 seek=512"
}
buffer_limit_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_init_test_cases() {
	atf_add_test_case existence
	atf_add_test_case help_device
	atf_add_test_case stats_check
	atf_add_test_case man_page
	atf_add_test_case write_read
	atf_add_test_case offset_test
	atf_add_test_case buffer_limit
}
