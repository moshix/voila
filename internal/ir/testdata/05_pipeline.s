*------------------------------------------------------------------------------
*        VOILA VM ASSEMBLY LISTING
*        SOURCE: 05_pipeline.voi
*
*        REGISTER-BASED LINEAR IR (SPEC #12). THIS IS THE FORM
*        THE C BACKEND CONSUMES; IT IS NOT EXECUTED DIRECTLY.
*        ENTRY ORDER: $INIT, $SCRIPT, MAIN. ^NAME DENOTES AN
*        UPVALUE OF THE ENCLOSING FRAME: A CLOSURE CAPTURES THAT
*        FRAME BY REFERENCE, NOT ITS VALUES BY COPY.
*------------------------------------------------------------------------------
*
*        CONSTANT POOL
K0      DC       INT    1
K1      DC       INT    2
K2      DC       INT    0
K3      DC       INT    4
K4      DC       INT    12
K5      DC       BOOL   true
K6      DC       STR    'even square #%d: %d\n'
K7      DC       BOOL   false
K8      DC       UNIT   ()
K9      DC       STR    'pipeline sum:'
K10     DC       INT    40
K11     DC       STR    'never printed'
K12     DC       STR    'stuck stage reaped by timeout; suppressed:'
K13     DC       STR    'done'
*
*        generator
        CSECT                                   * func generator(out chan[int]…
000000           LOADK    r2,K0                 * 13| for i in 1..=n {
000004           NEWRANGE r3,r2,r1,1,incl
000008           ITER     r4,r3
00000C  L1       ITNEXT   r5,r6,r4,L2
000010           SEND     r0,r6                 * 14| out <- i
000014           JUMP     L1
000018  L2       CALL     r7,close,(r0)         * 16| close(out)
00001C           RET      
*
squarer CSECT                                   * func squarer(inp chan[int], …
000000           ITER     r2,r0                 * 20| each v in inp {
000004  L1       ITNEXT   r3,r4,r2,L2
000008           MUL      r5,r4,r4              * 21| out <- v * v
00000C           SEND     r1,r5
000010           JUMP     L1
000014  L2       CALL     r6,close,(r1)         * 23| close(out)
000018           RET      
*
*        keep_even
        CSECT                                   * func keep_even(inp chan[int]…
000000           ITER     r2,r0                 * 27| each v in inp {
000004  L1       ITNEXT   r3,r4,r2,L2
000008           LOADK    r5,K1                 * 28| if v % 2 == 0 {
00000C           MOD      r6,r4,r5
000010           LOADK    r7,K2
000014           CMPEQ    r8,r6,r7
000018           JMPF     r8,L3
00001C           SEND     r1,r4                 * 29| out <- v
000020  L3,L4    JUMP     L1
000024  L2       CALL     r9,close,(r1)         * 32| close(out)
000028           RET      
*
main    CSECT                                   * func main()  regs=34
000000           LOADK    r1,K3                 * 36| let raw      = chan[int]…
000004           NEWCHAN  r0,r1
000008           LOADK    r3,K3                 * 37| let squared  = chan[int]…
00000C           NEWCHAN  r2,r3
000010           LOADK    r5,K3                 * 38| let filtered = chan[int]…
000014           NEWCHAN  r4,r5
000018           LOADK    r7,K0                 * 39| let sum_ch   = chan[int]…
00001C           NEWCHAN  r6,r7
000020           GRPBEG                         * 41| group {
000024           LOADK    r8,K4                 * 42| spawn generator(raw, 12)
000028           SPAWN    r9,generator,(r0,r8)
00002C           SPAWN    r10,squarer,(r0,r2)   * 43| spawn squarer(raw, squar…
000030           SPAWN    r11,keep_even,(r2,r4) * 44| spawn keep_even(squared,…
000034           MKCLOS   r12,main$fn1          * 45| spawn {
000038           SPAWNR   r13,r12
00003C           GRPEND   
000040           LOADK    r14,K9                * 65| say "pipeline sum:", <-s…
000044           TOSTR    r15,r14
000048           RECV     r16,r6
00004C           TOSTR    r17,r16
000050           SAY      (r15,r17)
000054           LOADK    r19,K2                * 70| let stuck = chan[int](0)
000058           NEWCHAN  r18,r19
00005C           EHPUSH   L1                    * 71| try {
000060           LOADK    r20,K10               * 72| group timeout 40 * time.…
000064           LOADFN   r21,time.Millisecond
000068           MUL      r22,r20,r21
00006C           GRPBEG   r22
000070           MKCLOS   r23,main$fn2          * 73| spawn {
000074           SPAWNR   r24,r23
000078           GRPEND   
00007C           EHPOP    
000080           JUMP     L2
000084  L1       CATCH    r25                   * 71| try {
000088           ISTYPE   r26,r25,'Timeout'     * 78| } catch e: Timeout {
00008C           JMPF     r26,L3
000090           LOADK    r27,K12               * 79| say "stuck stage reaped …
000094           TOSTR    r28,r27
000098           CALLM    r29,r25,suppressed,()
00009C           CALL     r30,len,(r29)
0000A0           TOSTR    r31,r30
0000A4           SAY      (r28,r31)
0000A8           JUMP     L2
0000AC  L3       THROW    r25
0000B0  L2       LOADK    r32,K13               * 81| say "done"
0000B4           TOSTR    r33,r32
0000B8           SAY      (r33)
0000BC           RET      
*
*        main$fn1
        CSECT                                   * spawn body  regs=10
000000           LOADK    r0,K2                 * 46| var sum = 0
000004           LOADK    r1,K2                 * 47| var received = 0
000008           LOADK    r2,K5                 * 48| var open = true
00000C  L1       CKCANC                         * 49| while open {
000010           JMPF     r2,L2
000014           SELBEG                         * 50| select {
000018           SELRECV  L4,^filtered,v=r3,ok=r4  * 51| case v, ok := <-filte…
00001C           SELEND                         * 50| select {
000020  L4       JMPF     r4,L5                 * 52| if ok {
000024           LOADK    r5,K0                 * 53| received += 1
000028           ADD      r1,r1,r5
00002C           ADD      r0,r0,r3              * 54| sum += v
000030           LOADK    r6,K6                 * 55| fmt.printf("even square …
000034           CALL     r7,fmt.printf,(r6,r1,r3)
000038           JUMP     L6
00003C  L5       LOADK    r8,K7                 * 57| open = false
000040           MOVE     r2,r8
000044  L6       JUMP     L3
000048  L3       JUMP     L1
00004C  L2       SEND     ^sum_ch,r0            * 61| sum_ch <- sum
000050           LOADK    r9,K8
000054           RET      r9
*
*        main$fn2
        CSECT                                   * spawn body  regs=4
000000           RECV     r0,^stuck             * 74| let v = <-stuck      // …
000004           LOADK    r1,K11                * 75| say "never printed", v
000008           TOSTR    r2,r1
00000C           TOSTR    r3,r0
000010           SAY      (r2,r3)
000014           RET      
*
        END
