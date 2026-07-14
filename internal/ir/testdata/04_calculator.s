*------------------------------------------------------------------------------
*        VOILA VM ASSEMBLY LISTING
*        SOURCE: 04_calculator.voi
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
K1      DC       INT    32
K2      DC       INT    48
K3      DC       INT    57
K4      DC       BOOL   false
K5      DC       STR    'bad number '
K6      DC       INT    43
K7      DC       INT    45
K8      DC       INT    42
K9      DC       INT    47
K10     DC       INT    94
K11     DC       INT    40
K12     DC       INT    41
K13     DC       RUNE   '+'
K14     DC       RUNE   '-'
K15     DC       RUNE   '*'
K16     DC       RUNE   '/'
K17     DC       RUNE   '^'
K18     DC       STR    'unexpected character '
K19     DC       STR    ' at column '
K20     DC       INT    46
K21     DC       BOOL   true
K22     DC       UNIT   ()
K23     DC       FLOAT  0
K24     DC       STR    'division by zero'
K25     DC       STR    'missing closing parenthesis'
K26     DC       STR    'unexpected end of expression'
K27     DC       STR    'unexpected token'
K28     DC       INT    0
K29     DC       STR    'trailing garbage after expression'
K30     DC       STR    '2 + 3 * 4'
K31     DC       STR    '(2 + 3) * 4'
K32     DC       STR    '2 ^ 10'
K33     DC       STR    '2 ^ 3 ^ 2'
K34     DC       STR    '-(4 + 1) * 2'
K35     DC       STR    '100 / 8'
K36     DC       STR    '3.5 * 2 + 0.25'
K37     DC       STR    '((((7))))'
K38     DC       STR    '1 / 0'
K39     DC       STR    '2 +'
K40     DC       STR    '4 $ 2'
K41     DC       STR    '%-16s = %v\n'
K42     DC       STR    '%-16s ! %s\n'
*
*        ENUM Token
Token   DSECT
        DS       VARIANT  Num(value float)
        DS       VARIANT  Op(sym rune)
        DS       VARIANT  LParen
        DS       VARIANT  RParen
        DS       VARIANT  End
*
*        STRUCT Lexer
Lexer   DSECT
        DS       STR      src
        DS       INT      pos
*
*        STRUCT Parser
Parser  DSECT
        DS       LEXER    lx
        DS       TOKEN    cur
*
*        Lexer.peek_byte
        CSECT                                   * func peek_byte(self) int  re…
000000           FIELD    r1,r0,pos             * 25| if self.pos >= len(self.…
000004           FIELD    r2,r0,src
000008           CALL     r3,len,(r2)
00000C           CMPGE    r4,r1,r3
000010           JMPF     r4,L1
000014           LOADK    r5,K0
000018           NEG      r6,r5
00001C           RET      r6
000020  L1,L2    FIELD    r7,r0,src             * 26| return self.src[self.pos]
000024           FIELD    r8,r0,pos
000028           INDEX    r9,r7,r8
00002C           RET      r9
000030           RET      
*
*        Lexer.next
        CSECT                                   * func next(mut self) Token!  …
000000  L1       CKCANC                         * 31| while self.peek_byte() =…
000004           CALLM    r1,r0,peek_byte,()
000008           LOADK    r2,K1
00000C           CMPEQ    r3,r1,r2
000010           JMPF     r3,L2
000014           LOADK    r4,K0                 * 32| self.pos += 1
000018           FIELD    r5,r0,pos
00001C           ADD      r6,r5,r4
000020           SETFLD   r0,pos,r6
000024           JUMP     L1
000028  L2       CALLM    r7,r0,peek_byte,()    * 34| let c = self.peek_byte()
00002C           LOADK    r8,K0                 * 35| if c == -1 { return End }
000030           NEG      r9,r8
000034           CMPEQ    r10,r7,r9
000038           JMPF     r10,L3
00003C           NEWENUM  r11,Token.End,()
000040           RET      r11
000044  L3,L4    LOADK    r13,K2                * 38| if c >= 48 and c <= 57 {
000048           CMPGE    r14,r7,r13
00004C           JMPF     r14,L7
000050           LOADK    r15,K3
000054           CMPLE    r16,r7,r15
000058           MOVE     r12,r16
00005C           JUMP     L8
000060  L7       LOADK    r12,K4
000064  L8       JMPF     r12,L5
000068           FIELD    r17,r0,pos            * 39| let start = self.pos
00006C  L9       CKCANC                         * 40| while self.is_digit_or_d…
000070           CALLM    r18,r0,is_digit_or_dot,()
000074           JMPF     r18,L10
000078           LOADK    r19,K0                * 41| self.pos += 1
00007C           FIELD    r20,r0,pos
000080           ADD      r21,r20,r19
000084           SETFLD   r0,pos,r21
000088           JUMP     L9
00008C  L10      FIELD    r22,r0,src            * 43| let text = self.src[star…
000090           FIELD    r23,r0,pos
000094           NEWRANGE r24,r17,r23,1,excl
000098           SLICE    r25,r22,r24
00009C           CALL     r26,to_float,(r25)    * 44| let v = try to_float(tex…
0000A0           ISFAIL   r28,r26
0000A4           JMPT     r28,L11
0000A8           MOVE     r27,r26
0000AC           JUMP     L12
0000B0  L11      INTERP   r29,(K5,r25)
0000B4           CALL     r30,err,(r29)
0000B8           RET      r30
0000BC  L12      NEWENUM  r31,Token.Num,(r27)   * 45| return Num(v)
0000C0           RET      r31
0000C4  L5,L6    LOADK    r32,K0                * 48| self.pos += 1
0000C8           FIELD    r33,r0,pos
0000CC           ADD      r34,r33,r32
0000D0           SETFLD   r0,pos,r34
0000D4           LOADK    r35,K6                * 50| case 43:
0000D8           CMPEQ    r36,r7,r35            * 49| switch c {
0000DC           JMPT     r36,L14
0000E0           LOADK    r37,K7                * 52| case 45:
0000E4           CMPEQ    r38,r7,r37            * 49| switch c {
0000E8           JMPT     r38,L15
0000EC           LOADK    r39,K8                * 54| case 42:
0000F0           CMPEQ    r40,r7,r39            * 49| switch c {
0000F4           JMPT     r40,L16
0000F8           LOADK    r41,K9                * 56| case 47:
0000FC           CMPEQ    r42,r7,r41            * 49| switch c {
000100           JMPT     r42,L17
000104           LOADK    r43,K10               * 58| case 94:
000108           CMPEQ    r44,r7,r43            * 49| switch c {
00010C           JMPT     r44,L18
000110           LOADK    r45,K11               * 60| case 40:
000114           CMPEQ    r46,r7,r45            * 49| switch c {
000118           JMPT     r46,L19
00011C           LOADK    r47,K12               * 62| case 41:
000120           CMPEQ    r48,r7,r47            * 49| switch c {
000124           JMPT     r48,L20
000128           JUMP     L21
00012C  L14      LOADK    r49,K13               * 51| return Op('+')
000130           NEWENUM  r50,Token.Op,(r49)
000134           RET      r50
000138           JUMP     L13
00013C  L15      LOADK    r51,K14               * 53| return Op('-')
000140           NEWENUM  r52,Token.Op,(r51)
000144           RET      r52
000148           JUMP     L13
00014C  L16      LOADK    r53,K15               * 55| return Op('*')
000150           NEWENUM  r54,Token.Op,(r53)
000154           RET      r54
000158           JUMP     L13
00015C  L17      LOADK    r55,K16               * 57| return Op('/')
000160           NEWENUM  r56,Token.Op,(r55)
000164           RET      r56
000168           JUMP     L13
00016C  L18      LOADK    r57,K17               * 59| return Op('^')
000170           NEWENUM  r58,Token.Op,(r57)
000174           RET      r58
000178           JUMP     L13
00017C  L19      NEWENUM  r59,Token.LParen,()   * 61| return LParen
000180           RET      r59
000184           JUMP     L13
000188  L20      NEWENUM  r60,Token.RParen,()   * 63| return RParen
00018C           RET      r60
000190           JUMP     L13
000194  L21      CONV     r61,rune,r7           * 65| return err("unexpected c…
000198           NEWSLICE r62,(r61)
00019C           CALL     r63,str.from_runes,(r62)
0001A0           FIELD    r64,r0,pos
0001A4           INTERP   r65,(K18,r63,K19,r64)
0001A8           CALL     r66,err,(r65)
0001AC           RET      r66
0001B0           JUMP     L13
0001B4  L13      RET      
*
*        Lexer.is_digit_or_dot
        CSECT                                   * func is_digit_or_dot(self) b…
000000           CALLM    r1,r0,peek_byte,()    * 70| let c = self.peek_byte()
000004           LOADK    r2,K20                * 71| if c == 46 { return true…
000008           CMPEQ    r3,r1,r2
00000C           JMPF     r3,L1
000010           LOADK    r4,K21
000014           RET      r4
000018  L1,L2    LOADK    r6,K2                 * 72| return c >= 48 and c <= …
00001C           CMPGE    r7,r1,r6
000020           JMPF     r7,L3
000024           LOADK    r8,K3
000028           CMPLE    r9,r1,r8
00002C           MOVE     r5,r9
000030           JUMP     L4
000034  L3       LOADK    r5,K4
000038  L4       RET      r5
00003C           RET      
*
*        Parser.advance
        CSECT                                   * func advance(mut self) unit!…
000000           FIELD    r1,r0,lx              * 83| self.cur = try self.lx.n…
000004           CALLM    r2,r1,next,()
000008           TRYP     r3,r2
00000C           SETFLD   r0,cur,r3
000010           LOADK    r4,K22                * 84| return ()
000014           RET      r4
000018           RET      
*
*        Parser.expr
        CSECT                                   * func expr(mut self) float!  …
000000           CALLM    r1,r0,term,()         * 89| var left = try self.term…
000004           TRYP     r2,r1
000008  L1       CKCANC                         * 90| for {
00000C           FIELD    r3,r0,cur             * 91| match self.cur {
000010           PMATCH   r5,r3,'Op(sym)',(r4)  * 92| Op(sym) if sym == '+' =>…
000014           JMPF     r5,L4
000018           LOADK    r6,K13
00001C           CMPEQ    r7,r4,r6
000020           JMPF     r7,L4
000024           CALLM    r8,r0,advance,()      * 93| try self.advance()
000028           TRYP     r9,r8
00002C           CALLM    r10,r0,term,()        * 94| left = left + try self.t…
000030           TRYP     r11,r10
000034           ADD      r12,r2,r11
000038           MOVE     r2,r12
00003C           JUMP     L3
000040  L4       PMATCH   r14,r3,'Op(sym)',(r13)  * 96| Op(sym) if sym == '-' …
000044           JMPF     r14,L5
000048           LOADK    r15,K14
00004C           CMPEQ    r16,r13,r15
000050           JMPF     r16,L5
000054           CALLM    r17,r0,advance,()     * 97| try self.advance()
000058           TRYP     r18,r17
00005C           CALLM    r19,r0,term,()        * 98| left = left - try self.t…
000060           TRYP     r20,r19
000064           SUB      r21,r2,r20
000068           MOVE     r2,r21
00006C           JUMP     L3
000070  L5       PMATCH   r22,r3,'_',()         * 100| _ => return left
000074           JMPF     r22,L6
000078           RET      r2
00007C           JUMP     L3
000080  L6       ABORT    'no match arm matched'  * 91| match self.cur {
000084  L3       JUMP     L1
000088  L2       RET      
*
*        Parser.term
        CSECT                                   * func term(mut self) float!  …
000000           CALLM    r1,r0,power,()        * 107| var left = try self.pow…
000004           TRYP     r2,r1
000008  L1       CKCANC                         * 108| for {
00000C           FIELD    r3,r0,cur             * 109| match self.cur {
000010           PMATCH   r5,r3,'Op(sym)',(r4)  * 110| Op(sym) if sym == '*' =…
000014           JMPF     r5,L4
000018           LOADK    r6,K15
00001C           CMPEQ    r7,r4,r6
000020           JMPF     r7,L4
000024           CALLM    r8,r0,advance,()      * 111| try self.advance()
000028           TRYP     r9,r8
00002C           CALLM    r10,r0,power,()       * 112| left = left * try self.…
000030           TRYP     r11,r10
000034           MUL      r12,r2,r11
000038           MOVE     r2,r12
00003C           JUMP     L3
000040  L4       PMATCH   r14,r3,'Op(sym)',(r13)  * 114| Op(sym) if sym == '/'…
000044           JMPF     r14,L5
000048           LOADK    r15,K16
00004C           CMPEQ    r16,r13,r15
000050           JMPF     r16,L5
000054           CALLM    r17,r0,advance,()     * 115| try self.advance()
000058           TRYP     r18,r17
00005C           CALLM    r19,r0,power,()       * 116| let r = try self.power()
000060           TRYP     r20,r19
000064           LOADK    r21,K23               * 117| if r == 0.0 { return er…
000068           CMPEQ    r22,r20,r21
00006C           JMPF     r22,L6
000070           LOADK    r23,K24
000074           CALL     r24,err,(r23)
000078           RET      r24
00007C  L6,L7    DIV      r25,r2,r20            * 118| left = left / r
000080           MOVE     r2,r25
000084           JUMP     L3
000088  L5       PMATCH   r26,r3,'_',()         * 120| _ => return left
00008C           JMPF     r26,L8
000090           RET      r2
000094           JUMP     L3
000098  L8       ABORT    'no match arm matched'  * 109| match self.cur {
00009C  L3       JUMP     L1
0000A0  L2       RET      
*
*        Parser.power
        CSECT                                   * func power(mut self) float! …
000000           CALLM    r1,r0,unary,()        * 127| let base = try self.una…
000004           TRYP     r2,r1
000008           FIELD    r3,r0,cur             * 128| match self.cur {
00000C           PMATCH   r5,r3,'Op(sym)',(r4)  * 129| Op(sym) if sym == '^' =…
000010           JMPF     r5,L2
000014           LOADK    r6,K17
000018           CMPEQ    r7,r4,r6
00001C           JMPF     r7,L2
000020           CALLM    r8,r0,advance,()      * 130| try self.advance()
000024           TRYP     r9,r8
000028           CALLM    r10,r0,power,()       * 131| let e = try self.power()
00002C           TRYP     r11,r10
000030           POW      r12,r2,r11            * 132| return base ** e
000034           RET      r12
000038           JUMP     L1
00003C  L2       PMATCH   r13,r3,'_',()         * 134| _ => return base
000040           JMPF     r13,L3
000044           RET      r2
000048           JUMP     L1
00004C  L3       ABORT    'no match arm matched'  * 128| match self.cur {
000050  L1       RET      
*
*        Parser.unary
        CSECT                                   * func unary(mut self) float! …
000000           FIELD    r1,r0,cur             * 140| match self.cur {
000004           PMATCH   r3,r1,'Op(sym)',(r2)  * 141| Op(sym) if sym == '-' =…
000008           JMPF     r3,L2
00000C           LOADK    r4,K14
000010           CMPEQ    r5,r2,r4
000014           JMPF     r5,L2
000018           CALLM    r6,r0,advance,()      * 142| try self.advance()
00001C           TRYP     r7,r6
000020           CALLM    r8,r0,unary,()        * 143| let v = try self.unary()
000024           TRYP     r9,r8
000028           NEG      r10,r9                * 144| return -v
00002C           RET      r10
000030           JUMP     L1
000034  L2       PMATCH   r11,r1,'_',()         * 146| _ => return try self.pr…
000038           JMPF     r11,L3
00003C           CALLM    r12,r0,primary,()
000040           TRYP     r13,r12
000044           RET      r13
000048           JUMP     L1
00004C  L3       ABORT    'no match arm matched'  * 140| match self.cur {
000050  L1       RET      
*
*        Parser.primary
        CSECT                                   * func primary(mut self) float…
000000           FIELD    r1,r0,cur             * 152| match self.cur {
000004           PMATCH   r3,r1,'Num(v)',(r2)   * 153| Num(v) => {
000008           JMPF     r3,L2
00000C           CALLM    r4,r0,advance,()      * 154| try self.advance()
000010           TRYP     r5,r4
000014           RET      r2                    * 155| return v
000018           JUMP     L1
00001C  L2       PMATCH   r6,r1,'LParen',()     * 157| LParen => {
000020           JMPF     r6,L3
000024           CALLM    r7,r0,advance,()      * 158| try self.advance()
000028           TRYP     r8,r7
00002C           CALLM    r9,r0,expr,()         * 159| let v = try self.expr()
000030           TRYP     r10,r9
000034           FIELD    r11,r0,cur            * 160| match self.cur {
000038           PMATCH   r12,r11,'RParen',()   * 161| RParen => try self.adva…
00003C           JMPF     r12,L5
000040           CALLM    r13,r0,advance,()
000044           TRYP     r14,r13
000048           JUMP     L4
00004C  L5       PMATCH   r15,r11,'_',()        * 162| _      => return err("m…
000050           JMPF     r15,L6
000054           LOADK    r16,K25
000058           CALL     r17,err,(r16)
00005C           RET      r17
000060           JUMP     L4
000064  L6       ABORT    'no match arm matched'  * 160| match self.cur {
000068  L4       RET      r10                   * 164| return v
00006C           JUMP     L1
000070  L3       PMATCH   r18,r1,'End',()       * 166| End    => return err("u…
000074           JMPF     r18,L7
000078           LOADK    r19,K26
00007C           CALL     r20,err,(r19)
000080           RET      r20
000084           JUMP     L1
000088  L7       PMATCH   r21,r1,'_',()         * 167| _      => return err("u…
00008C           JMPF     r21,L8
000090           LOADK    r22,K27
000094           CALL     r23,err,(r22)
000098           RET      r23
00009C           JUMP     L1
0000A0  L8       ABORT    'no match arm matched'  * 152| match self.cur {
0000A4  L1       RET      
*
*        evaluate
        CSECT                                   * func evaluate(src str) float…
000000           LOADK    r1,K28                * 173| var p = Parser{lx: Lexe…
000004           NEWSTRUCT r2,Lexer,(src:r0,pos:r1)
000008           NEWENUM  r3,Token.End,()
00000C           NEWSTRUCT r4,Parser,(lx:r2,cur:r3)
000010           CALLM    r5,r4,advance,()      * 174| try p.advance()
000014           TRYP     r6,r5
000018           CALLM    r7,r4,expr,()         * 175| let v = try p.expr()
00001C           TRYP     r8,r7
000020           FIELD    r9,r4,cur             * 176| match p.cur {
000024           PMATCH   r10,r9,'End',()       * 177| End => return v
000028           JMPF     r10,L2
00002C           RET      r8
000030           JUMP     L1
000034  L2       PMATCH   r11,r9,'_',()         * 178| _   => return err("trai…
000038           JMPF     r11,L3
00003C           LOADK    r12,K29
000040           CALL     r13,err,(r12)
000044           RET      r13
000048           JUMP     L1
00004C  L3       ABORT    'no match arm matched'  * 176| match p.cur {
000050  L1       RET      
*
main    CSECT                                   * func main()  regs=25
000000           LOADK    r0,K30                * 184| "2 + 3 * 4",
000004           LOADK    r1,K31                * 185| "(2 + 3) * 4",
000008           LOADK    r2,K32                * 186| "2 ^ 10",
00000C           LOADK    r3,K33                * 187| "2 ^ 3 ^ 2",
000010           LOADK    r4,K34                * 188| "-(4 + 1) * 2",
000014           LOADK    r5,K35                * 189| "100 / 8",
000018           LOADK    r6,K36                * 190| "3.5 * 2 + 0.25",
00001C           LOADK    r7,K37                * 191| "((((7))))",
000020           LOADK    r8,K38                * 192| "1 / 0",
000024           LOADK    r9,K39                * 193| "2 +",
000028           LOADK    r10,K40               * 194| "4 $ 2",
00002C           NEWSLICE r11,(r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10)  * 183| let …
000030           ITER     r12,r11               * 196| each src in cases {
000034  L1       ITNEXT   r13,r14,r12,L2
000038           CALL     r15,evaluate,(r14)    * 197| match evaluate(src) {
00003C           PMATCH   r17,r15,'Ok(v)',(r16) * 198| Ok(v)  => fmt.printf("%…
000040           JMPF     r17,L4
000044           LOADK    r18,K41
000048           CALL     r19,fmt.printf,(r18,r14,r16)
00004C           JUMP     L3
000050  L4       PMATCH   r21,r15,'Err(e)',(r20)  * 199| Err(e) => fmt.printf(…
000054           JMPF     r21,L5
000058           LOADK    r22,K42
00005C           CALLM    r23,r20,message,()
000060           CALL     r24,fmt.printf,(r22,r14,r23)
000064           JUMP     L3
000068  L5       ABORT    'no match arm matched'  * 197| match evaluate(src) {
00006C  L3       JUMP     L1
000070  L2       RET      
*
        END
