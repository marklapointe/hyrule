#!/usr/libexec/atf-sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Mark LaPointe <mark@cloudbsd.org>

atf_test_case existence cleanup
existence_head() {
	atf_set "descr" "Verify that Hyrule device files are created"
	atf_set "require.user" "root"
}
existence_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	atf_check test -c /dev/hyrule/map
	atf_check test -c /dev/hyrule/world/map_config
	atf_check test -c /dev/hyrule/characters/link/location/move
	atf_check test -c /dev/hyrule/characters/link/stats/health
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
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	atf_check -o "match:Welcome to the Hyrule Kernel Module!" cat /dev/hyrule/help
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
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	atf_check -o "inline:100\n" cat /dev/hyrule/characters/link/stats/health
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
	atf_check -o "match:Hyrulian Kernel Interface" man -l $(atf_get_srcdir)/../hyrule.4
}
man_page_cleanup() { :; }

atf_test_case write_read cleanup
write_read_head() {
	atf_set "descr" "Verify writing and reading back"
	atf_set "require.user" "root"
}
write_read_body() {
	kldunload hyrule >/dev/null 2>&1 || true
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
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	atf_check -o "inline:Sword\n" sh -c "cat /dev/hyrule/characters/link/weapons/sword | tail -c +8"
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
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	# Try to write at offset 1024 (size 1024, max offset 1023)
	atf_check -s exit:27 perl -e 'use POSIX; open($f, "+>", "/dev/hyrule/characters/link/weapons/sword") or die $!; seek($f, 1024, 0) or POSIX::_exit($!); syswrite($f, "!") or POSIX::_exit($!);'
}
buffer_limit_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case map_display cleanup
map_display_head() {
	atf_set "descr" "Verify map display"
	atf_set "require.user" "root"
}
map_display_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	atf_check -o "match:--- Hyrule Map ---" cat /dev/hyrule/map
	atf_check -o "match:Link at: \(0, 0\)" cat /dev/hyrule/map
}
map_display_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case map_move cleanup
map_move_head() {
	atf_set "descr" "Verify Link movement"
	atf_set "require.user" "root"
}
map_move_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	echo "e" > /dev/hyrule/characters/link/location/move
	atf_check -o "match:Link at: \(1, 0\)" cat /dev/hyrule/map
	echo "down" > /dev/hyrule/characters/link/location/move
	atf_check -o "match:Link at: \(1, 1\)" cat /dev/hyrule/map
}
map_move_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case map_config cleanup
map_config_head() {
	atf_set "descr" "Verify map configuration"
	atf_set "require.user" "root"
}
map_config_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	printf "ffffffffffF" > /dev/hyrule/world/map_config
	atf_check perl -e '$c = `cat /dev/hyrule/world/map_config`; exit 0 if $c =~ /^f{10}\nF/; exit 1;'
}
map_config_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case invalid_move cleanup
invalid_move_head() {
	atf_set "descr" "Verify invalid movement command"
	atf_set "require.user" "root"
}
invalid_move_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	# "jump" is not valid, should return EINVAL (22)
	atf_check -s exit:22 perl -e 'use POSIX; open($f, ">", "/dev/hyrule/characters/link/location/move") or die $!; syswrite($f, "jump") or POSIX::_exit($!);'
}
invalid_move_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case boundary_move cleanup
boundary_move_head() {
	atf_set "descr" "Verify movement boundaries"
	atf_set "require.user" "root"
}
boundary_move_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	# Start at (0,0). Move left (west). Should be blocked.
	echo "w" > /dev/hyrule/characters/link/location/move
	atf_check -o "match:Link at: \(0, 0\)" cat /dev/hyrule/map
	# Move up (north). Should be blocked.
	echo "n" > /dev/hyrule/characters/link/location/move
	atf_check -o "match:Link at: \(0, 0\)" cat /dev/hyrule/map
}
boundary_move_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case all_map_symbols cleanup
all_map_symbols_head() {
	atf_set "descr" "Verify all map symbols"
	atf_set "require.user" "root"
}
all_map_symbols_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	# Configure map: fwedaxxxxx...
	atf_check sh -c 'printf "fwedaxxxxx" > /dev/hyrule/world/map_config'
	# display for 'fwedax' should be 'LT DF?' (L at 0,0)
	atf_check -o "match:LT DF\?" cat /dev/hyrule/map
}
all_map_symbols_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case map_config_spaces cleanup
map_config_spaces_head() {
	atf_set "descr" "Verify spaces in map config are ignored"
	atf_set "require.user" "root"
}
map_config_spaces_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	printf "f f f f f f f f f f\nw w w w w w w w w w" > /dev/hyrule/world/map_config
	atf_check -o "match:^ffffffffff$" sh -c "cat /dev/hyrule/world/map_config | head -n 1"
	atf_check -o "match:^wwwwwwwwww$" sh -c "cat /dev/hyrule/world/map_config | head -n 2 | tail -n 1"
}
map_config_spaces_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case empty_read cleanup
empty_read_head() {
	atf_set "descr" "Verify reading beyond value length"
	atf_set "require.user" "root"
}
empty_read_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	# Initial health "100\n" (4 bytes). Read at offset 4.
	atf_check -o "empty" sh -c "cat /dev/hyrule/characters/link/stats/health | tail -c +5"
}
empty_read_cleanup() {
	kldunload hyrule >/dev/null 2>&1 || true
}

atf_test_case link_death cleanup
link_death_head() {
	atf_set "descr" "Verify module unloads when Link dies"
	atf_set "require.user" "root"
}
link_death_body() {
	kldunload hyrule >/dev/null 2>&1 || true
	atf_check kldload $(atf_get_srcdir)/../hyrule.ko
	# Set health to 0
	atf_check sh -c 'echo 0 > /dev/hyrule/characters/link/stats/health'
	# Wait a bit for the taskqueue to run
	sleep 1
	# Module should be gone
	atf_check -s exit:1 kldstat -n hyrule
}
link_death_cleanup() {
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
	atf_add_test_case map_display
	atf_add_test_case map_move
	atf_add_test_case map_config
	atf_add_test_case invalid_move
	atf_add_test_case boundary_move
	atf_add_test_case all_map_symbols
	atf_add_test_case map_config_spaces
	atf_add_test_case empty_read
	atf_add_test_case link_death
}
