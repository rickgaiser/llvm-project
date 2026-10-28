; REQUIRES: asserts
; RUN: llc -mtriple=mips64el -mcpu=r5900 -target-abi=n32 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=R5900
; RUN: llc -mtriple=mips64el -mcpu=mips3 -target-abi=n32 -debug-only=mips-subtarget -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=MIPS3 --allow-empty
; Ensure the register-pressure DAG scheduler is reserved for very large
; computed-goto graphs, while a small indirect branch uses the
; existing source-order scheduler. Other MIPS CPUs are unaffected.
; R5900-DAG: R5900 DAG scheduler: source for small_dispatch
; R5900-DAG: R5900 DAG scheduler: register pressure for large_dispatch
; MIPS3-NOT: R5900 DAG scheduler:
define i32 @small_dispatch(ptr %target) {
entry:
  indirectbr ptr %target, [label %a, label %b]
a:
  ret i32 1
b:
  ret i32 2
}
define i32 @large_dispatch(ptr %target) {
entry:
  indirectbr ptr %target, [label %L0, label %L1, label %L2, label %L3, label %L4, label %L5, label %L6, label %L7, label %L8, label %L9, label %L10, label %L11, label %L12, label %L13, label %L14, label %L15, label %L16, label %L17, label %L18, label %L19, label %L20, label %L21, label %L22, label %L23, label %L24, label %L25, label %L26, label %L27, label %L28, label %L29, label %L30, label %L31, label %L32, label %L33, label %L34, label %L35, label %L36, label %L37, label %L38, label %L39, label %L40, label %L41, label %L42, label %L43, label %L44, label %L45, label %L46, label %L47, label %L48, label %L49, label %L50, label %L51, label %L52, label %L53, label %L54, label %L55, label %L56, label %L57, label %L58, label %L59, label %L60, label %L61, label %L62, label %L63, label %L64, label %L65, label %L66, label %L67, label %L68, label %L69, label %L70, label %L71, label %L72, label %L73, label %L74, label %L75, label %L76, label %L77, label %L78, label %L79, label %L80, label %L81, label %L82, label %L83, label %L84, label %L85, label %L86, label %L87, label %L88, label %L89, label %L90, label %L91, label %L92, label %L93, label %L94, label %L95, label %L96, label %L97, label %L98, label %L99, label %L100, label %L101, label %L102, label %L103, label %L104, label %L105, label %L106, label %L107, label %L108, label %L109, label %L110, label %L111, label %L112, label %L113, label %L114, label %L115, label %L116, label %L117, label %L118, label %L119, label %L120, label %L121, label %L122, label %L123, label %L124, label %L125, label %L126, label %L127, label %L128, label %L129, label %L130, label %L131, label %L132, label %L133, label %L134, label %L135, label %L136, label %L137, label %L138, label %L139, label %L140, label %L141, label %L142, label %L143, label %L144, label %L145, label %L146, label %L147, label %L148, label %L149, label %L150, label %L151, label %L152, label %L153, label %L154, label %L155, label %L156, label %L157, label %L158, label %L159, label %L160, label %L161, label %L162, label %L163, label %L164, label %L165, label %L166, label %L167, label %L168, label %L169, label %L170, label %L171, label %L172, label %L173, label %L174, label %L175, label %L176, label %L177, label %L178, label %L179, label %L180, label %L181, label %L182, label %L183, label %L184, label %L185, label %L186, label %L187, label %L188, label %L189, label %L190, label %L191, label %L192, label %L193, label %L194, label %L195, label %L196, label %L197, label %L198, label %L199, label %L200, label %L201, label %L202, label %L203, label %L204, label %L205, label %L206, label %L207, label %L208, label %L209, label %L210, label %L211, label %L212, label %L213, label %L214, label %L215, label %L216, label %L217, label %L218, label %L219, label %L220, label %L221, label %L222, label %L223, label %L224, label %L225, label %L226, label %L227, label %L228, label %L229, label %L230, label %L231, label %L232, label %L233, label %L234, label %L235, label %L236, label %L237, label %L238, label %L239, label %L240, label %L241, label %L242, label %L243, label %L244, label %L245, label %L246, label %L247, label %L248, label %L249, label %L250, label %L251, label %L252, label %L253, label %L254, label %L255, label %L256, label %L257, label %L258, label %L259, label %L260, label %L261, label %L262, label %L263, label %L264, label %L265, label %L266, label %L267, label %L268, label %L269, label %L270, label %L271, label %L272, label %L273, label %L274, label %L275, label %L276, label %L277, label %L278, label %L279, label %L280, label %L281, label %L282, label %L283, label %L284, label %L285, label %L286, label %L287, label %L288, label %L289, label %L290, label %L291, label %L292, label %L293, label %L294, label %L295, label %L296, label %L297, label %L298, label %L299, label %L300, label %L301, label %L302, label %L303, label %L304, label %L305, label %L306, label %L307, label %L308, label %L309, label %L310, label %L311, label %L312, label %L313, label %L314, label %L315, label %L316, label %L317, label %L318, label %L319, label %L320, label %L321, label %L322, label %L323, label %L324, label %L325, label %L326, label %L327, label %L328, label %L329, label %L330, label %L331, label %L332, label %L333, label %L334, label %L335, label %L336, label %L337, label %L338, label %L339, label %L340, label %L341, label %L342, label %L343, label %L344, label %L345, label %L346, label %L347, label %L348, label %L349, label %L350, label %L351, label %L352, label %L353, label %L354, label %L355, label %L356, label %L357, label %L358, label %L359, label %L360, label %L361, label %L362, label %L363, label %L364, label %L365, label %L366, label %L367, label %L368, label %L369, label %L370, label %L371, label %L372, label %L373, label %L374, label %L375, label %L376, label %L377, label %L378, label %L379, label %L380, label %L381, label %L382, label %L383, label %L384, label %L385, label %L386, label %L387, label %L388, label %L389, label %L390, label %L391, label %L392, label %L393, label %L394, label %L395, label %L396, label %L397, label %L398, label %L399, label %L400, label %L401, label %L402, label %L403, label %L404, label %L405, label %L406, label %L407, label %L408, label %L409, label %L410, label %L411, label %L412, label %L413, label %L414, label %L415, label %L416, label %L417, label %L418, label %L419, label %L420, label %L421, label %L422, label %L423, label %L424, label %L425, label %L426, label %L427, label %L428, label %L429, label %L430, label %L431, label %L432, label %L433, label %L434, label %L435, label %L436, label %L437, label %L438, label %L439, label %L440, label %L441, label %L442, label %L443, label %L444, label %L445, label %L446, label %L447, label %L448, label %L449, label %L450, label %L451, label %L452, label %L453, label %L454, label %L455, label %L456, label %L457, label %L458, label %L459, label %L460, label %L461, label %L462, label %L463, label %L464, label %L465, label %L466, label %L467, label %L468, label %L469, label %L470, label %L471, label %L472, label %L473, label %L474, label %L475, label %L476, label %L477, label %L478, label %L479, label %L480, label %L481, label %L482, label %L483, label %L484, label %L485, label %L486, label %L487, label %L488, label %L489, label %L490, label %L491, label %L492, label %L493, label %L494, label %L495, label %L496, label %L497, label %L498, label %L499, label %L500, label %L501, label %L502, label %L503, label %L504, label %L505, label %L506, label %L507, label %L508, label %L509, label %L510, label %L511]
L0:
  ret i32 0
L1:
  ret i32 1
L2:
  ret i32 2
L3:
  ret i32 3
L4:
  ret i32 4
L5:
  ret i32 5
L6:
  ret i32 6
L7:
  ret i32 7
L8:
  ret i32 8
L9:
  ret i32 9
L10:
  ret i32 10
L11:
  ret i32 11
L12:
  ret i32 12
L13:
  ret i32 13
L14:
  ret i32 14
L15:
  ret i32 15
L16:
  ret i32 16
L17:
  ret i32 17
L18:
  ret i32 18
L19:
  ret i32 19
L20:
  ret i32 20
L21:
  ret i32 21
L22:
  ret i32 22
L23:
  ret i32 23
L24:
  ret i32 24
L25:
  ret i32 25
L26:
  ret i32 26
L27:
  ret i32 27
L28:
  ret i32 28
L29:
  ret i32 29
L30:
  ret i32 30
L31:
  ret i32 31
L32:
  ret i32 32
L33:
  ret i32 33
L34:
  ret i32 34
L35:
  ret i32 35
L36:
  ret i32 36
L37:
  ret i32 37
L38:
  ret i32 38
L39:
  ret i32 39
L40:
  ret i32 40
L41:
  ret i32 41
L42:
  ret i32 42
L43:
  ret i32 43
L44:
  ret i32 44
L45:
  ret i32 45
L46:
  ret i32 46
L47:
  ret i32 47
L48:
  ret i32 48
L49:
  ret i32 49
L50:
  ret i32 50
L51:
  ret i32 51
L52:
  ret i32 52
L53:
  ret i32 53
L54:
  ret i32 54
L55:
  ret i32 55
L56:
  ret i32 56
L57:
  ret i32 57
L58:
  ret i32 58
L59:
  ret i32 59
L60:
  ret i32 60
L61:
  ret i32 61
L62:
  ret i32 62
L63:
  ret i32 63
L64:
  ret i32 64
L65:
  ret i32 65
L66:
  ret i32 66
L67:
  ret i32 67
L68:
  ret i32 68
L69:
  ret i32 69
L70:
  ret i32 70
L71:
  ret i32 71
L72:
  ret i32 72
L73:
  ret i32 73
L74:
  ret i32 74
L75:
  ret i32 75
L76:
  ret i32 76
L77:
  ret i32 77
L78:
  ret i32 78
L79:
  ret i32 79
L80:
  ret i32 80
L81:
  ret i32 81
L82:
  ret i32 82
L83:
  ret i32 83
L84:
  ret i32 84
L85:
  ret i32 85
L86:
  ret i32 86
L87:
  ret i32 87
L88:
  ret i32 88
L89:
  ret i32 89
L90:
  ret i32 90
L91:
  ret i32 91
L92:
  ret i32 92
L93:
  ret i32 93
L94:
  ret i32 94
L95:
  ret i32 95
L96:
  ret i32 96
L97:
  ret i32 97
L98:
  ret i32 98
L99:
  ret i32 99
L100:
  ret i32 100
L101:
  ret i32 101
L102:
  ret i32 102
L103:
  ret i32 103
L104:
  ret i32 104
L105:
  ret i32 105
L106:
  ret i32 106
L107:
  ret i32 107
L108:
  ret i32 108
L109:
  ret i32 109
L110:
  ret i32 110
L111:
  ret i32 111
L112:
  ret i32 112
L113:
  ret i32 113
L114:
  ret i32 114
L115:
  ret i32 115
L116:
  ret i32 116
L117:
  ret i32 117
L118:
  ret i32 118
L119:
  ret i32 119
L120:
  ret i32 120
L121:
  ret i32 121
L122:
  ret i32 122
L123:
  ret i32 123
L124:
  ret i32 124
L125:
  ret i32 125
L126:
  ret i32 126
L127:
  ret i32 127
L128:
  ret i32 128
L129:
  ret i32 129
L130:
  ret i32 130
L131:
  ret i32 131
L132:
  ret i32 132
L133:
  ret i32 133
L134:
  ret i32 134
L135:
  ret i32 135
L136:
  ret i32 136
L137:
  ret i32 137
L138:
  ret i32 138
L139:
  ret i32 139
L140:
  ret i32 140
L141:
  ret i32 141
L142:
  ret i32 142
L143:
  ret i32 143
L144:
  ret i32 144
L145:
  ret i32 145
L146:
  ret i32 146
L147:
  ret i32 147
L148:
  ret i32 148
L149:
  ret i32 149
L150:
  ret i32 150
L151:
  ret i32 151
L152:
  ret i32 152
L153:
  ret i32 153
L154:
  ret i32 154
L155:
  ret i32 155
L156:
  ret i32 156
L157:
  ret i32 157
L158:
  ret i32 158
L159:
  ret i32 159
L160:
  ret i32 160
L161:
  ret i32 161
L162:
  ret i32 162
L163:
  ret i32 163
L164:
  ret i32 164
L165:
  ret i32 165
L166:
  ret i32 166
L167:
  ret i32 167
L168:
  ret i32 168
L169:
  ret i32 169
L170:
  ret i32 170
L171:
  ret i32 171
L172:
  ret i32 172
L173:
  ret i32 173
L174:
  ret i32 174
L175:
  ret i32 175
L176:
  ret i32 176
L177:
  ret i32 177
L178:
  ret i32 178
L179:
  ret i32 179
L180:
  ret i32 180
L181:
  ret i32 181
L182:
  ret i32 182
L183:
  ret i32 183
L184:
  ret i32 184
L185:
  ret i32 185
L186:
  ret i32 186
L187:
  ret i32 187
L188:
  ret i32 188
L189:
  ret i32 189
L190:
  ret i32 190
L191:
  ret i32 191
L192:
  ret i32 192
L193:
  ret i32 193
L194:
  ret i32 194
L195:
  ret i32 195
L196:
  ret i32 196
L197:
  ret i32 197
L198:
  ret i32 198
L199:
  ret i32 199
L200:
  ret i32 200
L201:
  ret i32 201
L202:
  ret i32 202
L203:
  ret i32 203
L204:
  ret i32 204
L205:
  ret i32 205
L206:
  ret i32 206
L207:
  ret i32 207
L208:
  ret i32 208
L209:
  ret i32 209
L210:
  ret i32 210
L211:
  ret i32 211
L212:
  ret i32 212
L213:
  ret i32 213
L214:
  ret i32 214
L215:
  ret i32 215
L216:
  ret i32 216
L217:
  ret i32 217
L218:
  ret i32 218
L219:
  ret i32 219
L220:
  ret i32 220
L221:
  ret i32 221
L222:
  ret i32 222
L223:
  ret i32 223
L224:
  ret i32 224
L225:
  ret i32 225
L226:
  ret i32 226
L227:
  ret i32 227
L228:
  ret i32 228
L229:
  ret i32 229
L230:
  ret i32 230
L231:
  ret i32 231
L232:
  ret i32 232
L233:
  ret i32 233
L234:
  ret i32 234
L235:
  ret i32 235
L236:
  ret i32 236
L237:
  ret i32 237
L238:
  ret i32 238
L239:
  ret i32 239
L240:
  ret i32 240
L241:
  ret i32 241
L242:
  ret i32 242
L243:
  ret i32 243
L244:
  ret i32 244
L245:
  ret i32 245
L246:
  ret i32 246
L247:
  ret i32 247
L248:
  ret i32 248
L249:
  ret i32 249
L250:
  ret i32 250
L251:
  ret i32 251
L252:
  ret i32 252
L253:
  ret i32 253
L254:
  ret i32 254
L255:
  ret i32 255
L256:
  ret i32 256
L257:
  ret i32 257
L258:
  ret i32 258
L259:
  ret i32 259
L260:
  ret i32 260
L261:
  ret i32 261
L262:
  ret i32 262
L263:
  ret i32 263
L264:
  ret i32 264
L265:
  ret i32 265
L266:
  ret i32 266
L267:
  ret i32 267
L268:
  ret i32 268
L269:
  ret i32 269
L270:
  ret i32 270
L271:
  ret i32 271
L272:
  ret i32 272
L273:
  ret i32 273
L274:
  ret i32 274
L275:
  ret i32 275
L276:
  ret i32 276
L277:
  ret i32 277
L278:
  ret i32 278
L279:
  ret i32 279
L280:
  ret i32 280
L281:
  ret i32 281
L282:
  ret i32 282
L283:
  ret i32 283
L284:
  ret i32 284
L285:
  ret i32 285
L286:
  ret i32 286
L287:
  ret i32 287
L288:
  ret i32 288
L289:
  ret i32 289
L290:
  ret i32 290
L291:
  ret i32 291
L292:
  ret i32 292
L293:
  ret i32 293
L294:
  ret i32 294
L295:
  ret i32 295
L296:
  ret i32 296
L297:
  ret i32 297
L298:
  ret i32 298
L299:
  ret i32 299
L300:
  ret i32 300
L301:
  ret i32 301
L302:
  ret i32 302
L303:
  ret i32 303
L304:
  ret i32 304
L305:
  ret i32 305
L306:
  ret i32 306
L307:
  ret i32 307
L308:
  ret i32 308
L309:
  ret i32 309
L310:
  ret i32 310
L311:
  ret i32 311
L312:
  ret i32 312
L313:
  ret i32 313
L314:
  ret i32 314
L315:
  ret i32 315
L316:
  ret i32 316
L317:
  ret i32 317
L318:
  ret i32 318
L319:
  ret i32 319
L320:
  ret i32 320
L321:
  ret i32 321
L322:
  ret i32 322
L323:
  ret i32 323
L324:
  ret i32 324
L325:
  ret i32 325
L326:
  ret i32 326
L327:
  ret i32 327
L328:
  ret i32 328
L329:
  ret i32 329
L330:
  ret i32 330
L331:
  ret i32 331
L332:
  ret i32 332
L333:
  ret i32 333
L334:
  ret i32 334
L335:
  ret i32 335
L336:
  ret i32 336
L337:
  ret i32 337
L338:
  ret i32 338
L339:
  ret i32 339
L340:
  ret i32 340
L341:
  ret i32 341
L342:
  ret i32 342
L343:
  ret i32 343
L344:
  ret i32 344
L345:
  ret i32 345
L346:
  ret i32 346
L347:
  ret i32 347
L348:
  ret i32 348
L349:
  ret i32 349
L350:
  ret i32 350
L351:
  ret i32 351
L352:
  ret i32 352
L353:
  ret i32 353
L354:
  ret i32 354
L355:
  ret i32 355
L356:
  ret i32 356
L357:
  ret i32 357
L358:
  ret i32 358
L359:
  ret i32 359
L360:
  ret i32 360
L361:
  ret i32 361
L362:
  ret i32 362
L363:
  ret i32 363
L364:
  ret i32 364
L365:
  ret i32 365
L366:
  ret i32 366
L367:
  ret i32 367
L368:
  ret i32 368
L369:
  ret i32 369
L370:
  ret i32 370
L371:
  ret i32 371
L372:
  ret i32 372
L373:
  ret i32 373
L374:
  ret i32 374
L375:
  ret i32 375
L376:
  ret i32 376
L377:
  ret i32 377
L378:
  ret i32 378
L379:
  ret i32 379
L380:
  ret i32 380
L381:
  ret i32 381
L382:
  ret i32 382
L383:
  ret i32 383
L384:
  ret i32 384
L385:
  ret i32 385
L386:
  ret i32 386
L387:
  ret i32 387
L388:
  ret i32 388
L389:
  ret i32 389
L390:
  ret i32 390
L391:
  ret i32 391
L392:
  ret i32 392
L393:
  ret i32 393
L394:
  ret i32 394
L395:
  ret i32 395
L396:
  ret i32 396
L397:
  ret i32 397
L398:
  ret i32 398
L399:
  ret i32 399
L400:
  ret i32 400
L401:
  ret i32 401
L402:
  ret i32 402
L403:
  ret i32 403
L404:
  ret i32 404
L405:
  ret i32 405
L406:
  ret i32 406
L407:
  ret i32 407
L408:
  ret i32 408
L409:
  ret i32 409
L410:
  ret i32 410
L411:
  ret i32 411
L412:
  ret i32 412
L413:
  ret i32 413
L414:
  ret i32 414
L415:
  ret i32 415
L416:
  ret i32 416
L417:
  ret i32 417
L418:
  ret i32 418
L419:
  ret i32 419
L420:
  ret i32 420
L421:
  ret i32 421
L422:
  ret i32 422
L423:
  ret i32 423
L424:
  ret i32 424
L425:
  ret i32 425
L426:
  ret i32 426
L427:
  ret i32 427
L428:
  ret i32 428
L429:
  ret i32 429
L430:
  ret i32 430
L431:
  ret i32 431
L432:
  ret i32 432
L433:
  ret i32 433
L434:
  ret i32 434
L435:
  ret i32 435
L436:
  ret i32 436
L437:
  ret i32 437
L438:
  ret i32 438
L439:
  ret i32 439
L440:
  ret i32 440
L441:
  ret i32 441
L442:
  ret i32 442
L443:
  ret i32 443
L444:
  ret i32 444
L445:
  ret i32 445
L446:
  ret i32 446
L447:
  ret i32 447
L448:
  ret i32 448
L449:
  ret i32 449
L450:
  ret i32 450
L451:
  ret i32 451
L452:
  ret i32 452
L453:
  ret i32 453
L454:
  ret i32 454
L455:
  ret i32 455
L456:
  ret i32 456
L457:
  ret i32 457
L458:
  ret i32 458
L459:
  ret i32 459
L460:
  ret i32 460
L461:
  ret i32 461
L462:
  ret i32 462
L463:
  ret i32 463
L464:
  ret i32 464
L465:
  ret i32 465
L466:
  ret i32 466
L467:
  ret i32 467
L468:
  ret i32 468
L469:
  ret i32 469
L470:
  ret i32 470
L471:
  ret i32 471
L472:
  ret i32 472
L473:
  ret i32 473
L474:
  ret i32 474
L475:
  ret i32 475
L476:
  ret i32 476
L477:
  ret i32 477
L478:
  ret i32 478
L479:
  ret i32 479
L480:
  ret i32 480
L481:
  ret i32 481
L482:
  ret i32 482
L483:
  ret i32 483
L484:
  ret i32 484
L485:
  ret i32 485
L486:
  ret i32 486
L487:
  ret i32 487
L488:
  ret i32 488
L489:
  ret i32 489
L490:
  ret i32 490
L491:
  ret i32 491
L492:
  ret i32 492
L493:
  ret i32 493
L494:
  ret i32 494
L495:
  ret i32 495
L496:
  ret i32 496
L497:
  ret i32 497
L498:
  ret i32 498
L499:
  ret i32 499
L500:
  ret i32 500
L501:
  ret i32 501
L502:
  ret i32 502
L503:
  ret i32 503
L504:
  ret i32 504
L505:
  ret i32 505
L506:
  ret i32 506
L507:
  ret i32 507
L508:
  ret i32 508
L509:
  ret i32 509
L510:
  ret i32 510
L511:
  ret i32 511
}
