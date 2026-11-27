config-1.3.1.ini and config-2.0-rc1.ini are CONFIG.INI files written by those versions' own `pcfg_save`:
`git archive v1.3.1 gpsp` (and `git archive integ-2.0 gpsp` for 2.0-rc1) built with a four-line driver
(`pcfg_load(path); pcfg_save();`) against that tree's psp/config_psp.c, psp/ctl_map.c and
frontend-common/fe_util.c, fed a player's non-default choices (a different scale, filter, quality, wide
view, rewind length, nickname, controls; for 2.0-rc1 also Smooth hand-over, Steady pacing, Fourth core,
NES Wide view and WonderSwan smoothing turned off).  Used by test_config_migrate.c (run_host_tests.py).
