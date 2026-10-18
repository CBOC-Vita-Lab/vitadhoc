# Mario Tennis: Power Tour -- Multiplayer, P1 vs P2 singles
# (spec for pair_script.py: H = host only, J = join only)
wait 450
press START 4
wait 850
press START 4
wait 300
press START 4
wait 600
# main menu: Power Tour -> Exhibition -> Multiplayer
press RIGHT 4
wait 40
press RIGHT 4
wait 40
press A 4
wait 240
# "Press the A Button to start" (both)
press A 4
wait 300
evt linked
dump
# Combinations: P1 vs. P2
H press A 4
wait 400
H press A 4
wait 100
J press A 4
wait 200
J press A 4
wait 200
H press A 4
wait 200
H press A 4
wait 200
H press A 4
wait 200
wait 300
evt match
dump
# rallies: both swing (A) every ~20 frames and wander; 2 games, 1 set ends
# the match around f12000 (desktop) -- the slice stops before the results
repeat 60
press A 4
wait 14
hold LEFT 12
press A 4
wait 14
hold RIGHT 12
press A 4
wait 14
hold UP 8
press A 4
wait 14
hold DOWN 8
endrepeat
wait 300
dump
evt match_slice_done
