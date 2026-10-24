# Pokemon Emerald -- Cable Club COLOSSEUM single battle (multigame fixture emb)
# (spec for pair_script.py: H = host only, J = join only)
mash A 4 0x030022C4 0xFFFFFFFF 0x080AAB2D 4800
evt title_ok
mash START 4 0x030022C4 0xFFFFFFFF 0x0802F6B1 3600
mash A 4 0x030022C4 0xFFFFFFFF 0x08085E5D 4800
evt overworld_ok
wait 30
holdmash DOWN  B 2 0x02037362 0xFFFF 12 900
holdmash RIGHT B 2 0x02037360 0xFFFF 17 900
holdmash UP    B 2 0x02037362 0xFFFF 11 900
press UP 2
evt at_direct_corner
logptr loc 2 0x03005D8C 0x04
# Cable Club attendant: COLOSSEUM (entry 1) -> SINGLE BATTLE (entry 0);
# she saves first
wait 30
press A 4
wait 150
press A 4
wait 150
press DOWN 4
wait 30
press A 4
wait 150
press A 4
wait 60
mashsram A 4800
evt entry_saved
# A through "please wait" until the warp to the colosseum
mashptrne A 2 0x03005D8C 0x04 0xFFFF 0x060A 4800
evt in_colosseum
waitram 4 0x030022C4 0xFFFFFFFF 0x08085E5D 3600
wait 120
# spawn live (13,15) host / (14,15) join; the spots are map (3,5) and
# (10,5) = live (10,12) for the host (slot-1 avatar), (17,12) for the join
H holdmash UP B 2 0x02037386 0xFFFF 12 300
H holdmash LEFT B 2 0x02037384 0xFFFF 10 300
J holdmash UP B 2 0x020373AA 0xFFFF 12 300
J holdmash RIGHT B 2 0x020373A8 0xFFFF 17 300
evt on_spot
# both on their spots: the battle (callback2 = BattleMainCB2)
waitram 4 0x030022C4 0xFFFFFFFF 0x08038421 7200
evt battle_started
dump
# turns: the local battler (host 0 at 0x03005D60, join 1 at 0x03005D64)
# reaches HandleInputChooseAction -> A (FIGHT) -> HandleInputChooseMove ->
# A (the first move); every turn's choices cross the cable before it plays.
# Three turns: both mons keep a wide HP margin, so neither faints whatever
# the battle RNG does (x86 and the PSP's MIPS dynarec are different engines:
# after SIX turns on the desktop the HP was 5 and 1, and in PPSSPP a mon
# fainted, the battle ended and the 7th prompt never came)
repeat 3
H waitram 4 0x03005D60 0xFFFFFFFF 0x08057589 10800
J waitram 4 0x03005D64 0xFFFFFFFF 0x08057589 10800
evt turn
H mashne A 4 0x03005D60 0xFFFFFFFF 0x08057589 300
J mashne A 4 0x03005D64 0xFFFFFFFF 0x08057589 300
H waitram 4 0x03005D60 0xFFFFFFFF 0x08057BFD 10800
J waitram 4 0x03005D64 0xFFFFFFFF 0x08057BFD 10800
H mashne A 4 0x03005D60 0xFFFFFFFF 0x08057BFD 300
J mashne A 4 0x03005D64 0xFFFFFFFF 0x08057BFD 300
evt move_chosen
endrepeat
H waitram 4 0x03005D60 0xFFFFFFFF 0x08057589 10800
J waitram 4 0x03005D64 0xFFFFFFFF 0x08057589 10800
evt turns_done
logram hp_player 2 0x02024542
logram hp_enemy 2 0x0202479A
dump
wait 300
