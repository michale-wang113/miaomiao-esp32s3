# Servo live sweep via JTAG - corrected addresses (LEDC @ 0x60019000)
# Timer0 is vendor's own: 80MHz APB, div 50000, res 13-bit = exact 50Hz.
# Servo pulse widths in 13-bit ticks (period 8192 = 20ms):
#   500us=205(0x1990 reg) 1500us=614(0x2660) 2000us=819(0x3330) 2500us=1024(0x4000)
source [find board/esp32s3-builtin.cfg]
init
halt 3000
echo "-- wait for light-sleep wake cycle --"
sleep 600
echo "-- unlock+disable RTC WDT --"
mww 0x600080b0 0x50d83aa1
mww 0x60008098 0x00000000
echo "-- disable task WDT (TIMG0) and int WDT (TIMG1) --"
mww 0x6001f064 0x50d83aa1
mww 0x6001f048 0x00000000
mww 0x60020064 0x50d83aa1
mww 0x60020048 0x00000000
echo "-- capture vendor state --"
mdw 0x600190a0 1
mdw 0x60019008 1
mdw 0x6001901c 1
echo "-- CH0+CH1 neutral 1500us (0x2660) --"
mww 0x60019008 0x00002660
mww 0x6001900c 0xc0100400
mww 0x6001901c 0x00002660
mww 0x60019020 0xc0100400
sleep 1500
echo "-- verify duty reg --"
mdw 0x60019010 1
mdw 0x60019024 1
echo "-- sweep to 0deg 500us (0x1990) --"
mww 0x60019008 0x00001990
mww 0x6001900c 0xc0100400
mww 0x6001901c 0x00001990
mww 0x60019020 0xc0100400
sleep 1500
echo "-- sweep to 180deg 2500us (0x4000) --"
mww 0x60019008 0x00004000
mww 0x6001900c 0xc0100400
mww 0x6001901c 0x00004000
mww 0x60019020 0xc0100400
sleep 1500
echo "-- back to neutral 1500us --"
mww 0x60019008 0x00002660
mww 0x6001900c 0xc0100400
mww 0x6001901c 0x00002660
mww 0x60019020 0xc0100400
sleep 1200
echo "-- restore vendor pose: CH0=0x3330(2000us) CH1=0x2660(1500us) --"
mww 0x60019008 0x00003330
mww 0x6001900c 0xc0100400
mww 0x6001901c 0x00002660
mww 0x60019020 0xc0100400
mdw 0x60019010 1
mdw 0x60019024 1
resume
shutdown
