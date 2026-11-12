| Test ROM | fps (no profiling) | ms/frame | CPU 65816 | APU | PPU | chip | frontend | PPU flushes/frame | APU port accesses/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Hello World (text) | 2726.3 | 0.37 | 44.4% | 37.1% | 18.2% | 0.0% | 0.3% | 1.0 | 0.0 |
| Mode 7 RotZoom | 1978.1 | 0.51 | 35.9% | 29.2% | 33.1% | 0.0% | 1.8% | 1.0 | 0.0 |
| Mode 7 Perspective | 2049.0 | 0.49 | 30.6% | 36.4% | 32.3% | 0.0% | 0.7% | 1.0 | 0.0 |
| Mode 7 StarWars crawl | 1722.6 | 0.58 | 31.8% | 22.5% | 44.8% | 0.0% | 0.9% | 1.8 | 0.0 |
| Mode 7 HDMA | 2039.9 | 0.49 | 32.8% | 29.1% | 37.8% | 0.0% | 0.3% | 1.0 | 0.0 |
| Mode 7 PlotLine (CPU draw) | 2024.8 | 0.49 | 37.3% | 29.7% | 30.4% | 0.0% | 2.5% | 1.0 | 0.0 |
| Mode 7 PlotPixel (CPU draw) | 2036.2 | 0.49 | 34.2% | 30.3% | 32.6% | 0.0% | 2.9% | 1.0 | 0.0 |
| Windows + HDMA | 2351.4 | 0.43 | 34.2% | 32.1% | 30.0% | 0.0% | 3.8% | 5.0 | 0.0 |
| HDMA wave | 2100.1 | 0.48 | 40.0% | 31.3% | 27.5% | 0.0% | 1.2% | 1.0 | 0.0 |
| Hi-colour 3840 (blend, HDMA) | 1422.3 | 0.70 | 18.9% | 25.7% | 55.1% | 0.0% | 0.2% | 1.0 | 0.0 |
| Pseudo hi-res 64 per row | 942.1 | 1.06 | 18.5% | 13.9% | 67.4% | 0.0% | 0.1% | 214.2 | 0.0 |
| Interlace RPG (hi-res 512x448) | 817.5 | 1.22 | 12.0% | 15.9% | 71.2% | 0.0% | 1.0% | 1.1 | 0.0 |
| Mosaic mode 5 (hi-res) | 566.3 | 1.77 | 9.6% | 11.7% | 77.8% | 0.0% | 0.9% | 1.0 | 0.0 |
| Rings (BG + windows) | 1504.6 | 0.66 | 28.5% | 25.1% | 45.3% | 0.0% | 1.2% | 1.0 | 0.0 |
| Sprites (object dropout test) | 1923.3 | 0.52 | 31.6% | 27.7% | 39.8% | 0.0% | 0.9% | 1.0 | 0.0 |
| Windows (shapes) | 2197.7 | 0.46 | 31.0% | 37.6% | 26.8% | 0.0% | 4.6% | 74.2 | 0.0 |
| Mode 7 tilemap scroll | 1947.1 | 0.51 | 27.9% | 35.9% | 35.3% | 0.0% | 1.0% | 1.0 | 0.0 |
| HDMA parallax (double buffered) | 2533.1 | 0.39 | 38.7% | 48.5% | 10.3% | 0.0% | 2.5% | 1.0 | 0.0 |
| HDMA shear (indirect) | 2333.8 | 0.43 | 44.7% | 36.1% | 17.6% | 0.0% | 1.5% | 1.0 | 0.0 |
| Monster Farm Jump (game) | 1253.5 | 0.80 | 20.5% | 23.5% | 55.2% | 0.0% | 0.7% | 1.6 | 0.0 |
| Super FX 8bpp PlotPixel 256x192 | 230.9 | 4.33 | 92.1% | 5.6% | 1.9% | 0.2% | 0.1% | 13.3 | 0.0 |
| Super FX 8bpp FillPoly 256x192 | 214.0 | 4.67 | 92.0% | 5.2% | 2.4% | 0.3% | 0.1% | 14.8 | 0.0 |
| Super FX 4bpp FillPoly 256x192 | 214.6 | 4.66 | 92.2% | 4.9% | 2.5% | 0.2% | 0.2% | 15.0 | 0.0 |
| Super FX 2bpp PlotLine 256x192 | 214.6 | 4.66 | 91.7% | 5.6% | 2.1% | 0.1% | 0.5% | 22.3 | 0.0 |
| SPC700 Axel-F | 3015.1 | 0.33 | 42.7% | 48.9% | 7.6% | 0.0% | 0.9% | 1.0 | 422.9 |
| SPC700 FF7 Prelude | 3024.3 | 0.33 | 46.2% | 44.6% | 7.8% | 0.0% | 1.3% | 1.0 | 434.9 |
| SPC700 Speech synth | 3027.7 | 0.33 | 45.4% | 44.5% | 8.4% | 0.0% | 1.8% | 1.0 | 572.4 |
| SPC700 Pitch mod | 2958.0 | 0.34 | 33.4% | 55.5% | 9.9% | 0.0% | 1.2% | 1.0 | 64.1 |
| SPC700 Noise | 3115.4 | 0.32 | 46.9% | 43.0% | 7.9% | 0.0% | 2.3% | 1.0 | 3.4 |
| CPU test ADC | 2555.0 | 0.39 | 37.7% | 43.8% | 18.1% | 0.0% | 0.4% | 1.0 | 0.0 |
| Synthetic Super FX (GSU saturated) | 814.9 | 1.23 | 15.4% | 11.9% | 3.9% | 67.5% | 1.2% | 1.0 | 0.0 |
| Synthetic SA-1 (saturated) | 1404.7 | 0.71 | 35.6% | 8.6% | 3.3% | 52.4% | 0.1% | 1.0 | 0.0 |

## Audio pacing simulation (audio_sim.sh, virtual time, synthetic 440/660 Hz tone)

Run with `sh snes/twin/audio_sim.sh --md`. No audio device: `snes_twin - --audio-sim S1..S7|all --tone [--legacy]` drives
`frontend/audio_pace.cpp` (the code the Vita player links) against a 59.94 Hz display, one emulated frame per refresh
(NTSC 60.0988 Hz, so the sound arrives 0.265% slow), a device clock with an offset and per-pass cost scenarios.
Fill and ratio columns are over the run after the first 6 s. Underruns count the whole run, start-up included. The click
detector looks for an output step far larger than the steps around it; a repeated (held) sample is not a step, so for the
legacy mode the underrun count is the measure of crackle.

Scenarios and pass criteria:

| | Scenario | Pass criterion |
|---|---|---|
| S1 | steady, 8 ms passes | 0 underruns, 0 clicks |
| S2+ / S2- | device clock +500 / -500 ppm, 10 virtual minutes | 0 underruns, 0 clicks |
| S3 | production exact until 15 s, then -0.265% | 0 underruns, 0 clicks, smoothed fill within 10% of the target from 10 s after the step on |
| S4 | an 80 ms stall every 5 s, 120 s | at most 1 underrun in total, 0 clicks |
| S5 | every pass 18.5 ms (free running, about 54 passes/s) | 0 clicks, mean ratio within 1% of the measured speed in the last 20 s, no underrun after 20 s |
| S6 | 150 ms burst of 25 ms passes every second, else 8 ms | 0 clicks, no underrun after 10 s (no number was given for S6) |
| S7 | pause 2 s at 10 s and 0.25 s at 20 s | 0 underruns, 0 clicks |

Legacy = the first player's audio (`audio_legacy=1` in threads.ini): 1024-frame grains, P-only rate control on the post-push
fill with a 60 ms target, hold the last sample on underrun. Its S1/S2 underruns are its start-up: it starts playing with
an empty queue. PI gains of the new pacer: Kp 40000 ppm and Ki 150 ppm/grain per unit of relative fill error (see the
note below); limits +-5000 / +-4000 ppm as planned.

#### New pacer

| Scenario | Result | Underruns | Clicks | Fill ms min/avg/max | Ratio ppm min/avg/max | Trims | Detail |
|---|---|---:|---:|---|---|---:|---|
| S1 | PASS | 0 | 0 | 55.6/67.4/79.7 | -4453/-2554/945 | 0 |  |
| S2+ | PASS | 0 | 0 | 40.7/52.4/79.3 | -5151/-3109/1895 | 0 |  |
| S2- | PASS | 0 | 0 | 40.6/52.4/80.2 | -3877/-2108/2499 | 0 |  |
| S3 | PASS | 0 | 0 | 55.7/68.2/82.5 | -3967/-1747/1858 | 0 | after step+10s: worst /ema-target/=6.9% (end 64.9 vs 65.0 ms) |
| S4 | PASS | 1 | 0 | 13.8/83.5/126.0 | -37266/-15908/-1943 | 0 |  |
| S5 | PASS | 5 | 0 | 73.0/133.9/155.3 | -108675/-101627/-98999 | 0 | mean ratio 0.8994 vs speed 0.8994 (/diff/ 0.00%);  |
| S6 | PASS | 2 | 0 | 58.2/108.3/138.8 | -44565/-35923/-29933 | 0 |  |
| S7 | PASS | 0 | 0 | 42.4/68.0/79.7 | -9000/-4490/-1657 | 0 |  |

#### Legacy (first player's audio)

| Scenario | Result | Underruns | Clicks | Fill ms min/avg/max | Ratio ppm min/avg/max | Trims | Detail |
|---|---|---:|---:|---|---|---:|---|
| S1 | FAIL | 2 | 0 | 29.6/43.4/52.5 | -5063/-2768/-1251 | 0 | 2 underruns;  |
| S2+ | FAIL | 2 | 0 | 27.7/41.1/49.5 | -5386/-3150/-1745 | 0 | 2 underruns;  |
| S2- | FAIL | 2 | 0 | 31.6/47.1/55.5 | -4736/-2157/-756 | 0 | 2 underruns;  |
| S3 | FAIL | 1 | 0 | 36.0/48.4/65.3 | -4002/-1926/882 | 0 | fill not at target (>10%): 1 underruns; after step+10s: worst /ema-target/=27.4% (end 43.5 vs 60.0 ms) |
| S4 | FAIL | 35 | 0 | 0.1/36.9/51.6 | -7217/-3774/-1401 | 0 | 35 underruns (max 1);  |
| S5 | FAIL | 1521 | 0 | 16.7/22.7/33.4 | -7217/-6220/-4439 | 0 | pitch does not follow speed: mean ratio 0.9938 vs speed 0.8994 (/diff/ 9.44%); 1005 underruns after 20 s;  |
| S6 | FAIL | 281 | 0 | 0.1/41.4/62.0 | -7217/-3005/331 | 0 | 232 underruns after 10 s;  |
| S7 | FAIL | 3 | 0 | 21.1/39.9/52.7 | -6484/-3351/-1214 | 0 | 3 underruns;  |

#### New pacer with the gains first planned (Kp 5000, Ki 40): S3 fails

Those gains give a lightly damped loop (damping about 0.15, period about 27 s): the smoothed fill rings around the target for minutes after any disturbance (S1 fill 47-83 ms, S3 22% off after 10 s). Kp 40000 / Ki 150 is damped about 0.4-0.5 with a settling time of a few seconds and is the default; the 60 Hz ripple on the ratio stays under 300 ppm. Result with the planned gains:

```
SIM S1 mode=new src=tone dur=60s result=PASS underruns=0 clicks=0 fill_ms=47.4/66.9/82.9 ratio_ppm=-5118/-2833/-1603 trims=0 dropped=0 | 
SIM S2+ mode=new src=tone dur=600s result=PASS underruns=0 clicks=0 fill_ms=38.5/52.2/81.3 ratio_ppm=-5285/-3136/-1687 trims=0 dropped=0 | 
SIM S2- mode=new src=tone dur=600s result=PASS underruns=0 clicks=0 fill_ms=38.4/52.4/84.8 ratio_ppm=-4998/-2132/-742 trims=0 dropped=0 | 
SIM S3 mode=new src=tone dur=45s result=FAIL underruns=0 clicks=0 fill_ms=46.7/68.4/92.2 ratio_ppm=-4879/-1959/2002 trims=0 dropped=0 | fill not at target (>10%): after step+10s: worst |ema-target|=21.5% (end 64.3 vs 65.0 ms)
SIM S4 mode=new src=tone dur=120s result=PASS underruns=1 clicks=0 fill_ms=16.0/83.4/141.9 ratio_ppm=-34617/-15927/-5883 trims=0 dropped=0 | 
SIM S5 mode=new src=tone dur=60s result=PASS underruns=5 clicks=0 fill_ms=53.1/127.2/168.3 ratio_ppm=-106797/-101846/-97097 trims=0 dropped=0 | mean ratio 0.9002 vs speed 0.8994 (|diff| 0.09%); 
SIM S6 mode=new src=tone dur=60s result=PASS underruns=2 clicks=0 fill_ms=23.2/106.2/155.4 ratio_ppm=-42521/-36895/-31385 trims=0 dropped=0 | 
SIM S7 mode=new src=tone dur=30s result=PASS underruns=0 clicks=0 fill_ms=42.2/60.6/76.6 ratio_ppm=-5475/-4685/-4033 trims=0 dropped=0 | 
```
