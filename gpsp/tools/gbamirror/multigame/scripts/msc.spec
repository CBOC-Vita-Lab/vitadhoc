# Mario Kart: Super Circuit -- Multiplayer > Multi-Pak link > VS (2 players,
# no CPU karts), 50 cc, random characters, Mushroom Cup > Peach Circuit.
# (spec for pair_script.py; gen_msc.py appends the closed-loop race driver)
# title: SINGLE PLAYER -> MULTIPLAYER
wait 900
press DOWN 4
wait 40
press A 4
# MULTI-PAK LINK (each GBA has a cartridge), then the cable handshake
wait 700
press A 8
wait 1100
# the parent's "OK?" (P1 + P2 found)
H press A 8
wait 400
evt linked
dump
# Choose a game: Mario GP -> VS
H press RIGHT 8
wait 120
H press A 8
wait 400
# 50 cc
H press A 8
wait 600
# Choose a character: both take RANDOM (the cursor's start), the parent's OK?
H press A 8
wait 200
J press A 8
wait 300
H press A 8
wait 100
H press A 8
wait 100
H press A 8
wait 300
# Choose a cup (Mushroom) -> a track (Peach Circuit)
H press A 8
wait 300
H press A 8
wait 300
H press A 8
wait 300
H press A 8
evt race_loading
wait 600
dump
evt race
