# Отчёт l2-db-spider

- Каталог: `./spider-output`
- Сгенерировано: 2026-11-08 15:07:08
- Файлов JSON: **12**

> Этот документ — структурированный вход для AI-агента. На его основе агент формирует сводный отчёт с рекомендациями.

## Сводка по БД и запросам

| БД | Запрос | Статус | Строк | Время генерации |
|----|--------|--------|------:|------------------|
| ccm2 | shared_pool | ok | 10086 | 2026-11-08 15:04:56 |
| ccm2 | table_sizes | ok | 135 | 2026-11-08 15:04:57 |
| ccm2 | tablespaces | ok | 40 | 2026-11-08 15:04:57 |
| ccm3t | shared_pool | error |  | 2026-11-08 11:01:13 |
| ccm3t | table_sizes | ok | 0 | 2026-11-08 11:01:13 |
| ccm3t | tablespaces | error |  | 2026-11-08 11:01:13 |
| ccm4 | shared_pool | ok | 63832 | 2026-11-08 15:04:57 |
| ccm4 | table_sizes | ok | 266 | 2026-11-08 15:04:59 |
| ccm4 | tablespaces | ok | 32 | 2026-11-08 15:04:59 |
| ccm5 | shared_pool | ok | 16200 | 2026-11-08 15:05:00 |
| ccm5 | table_sizes | ok | 142 | 2026-11-08 15:05:01 |
| ccm5 | tablespaces | ok | 36 | 2026-11-08 15:05:00 |

---

## ccm2 / shared_pool

_Файл_: `./spider-output/ccm2_shared_pool.json`, строк: 10086

_Колонки_: SQL_TEXT (VARCHAR2), SQL_ID (VARCHAR2), SHARABLE_MEM (NUMBER), PERSISTENT_MEM (NUMBER), RUNTIME_MEM (NUMBER), SORTS (NUMBER), VERSION_COUNT (NUMBER), LOADED_VERSIONS (NUMBER), OPEN_VERSIONS (NUMBER), USERS_OPENING (NUMBER), FETCHES (NUMBER), EXECUTIONS (NUMBER), PX_SERVERS_EXECUTIONS (NUMBER), END_OF_FETCH_COUNT (NUMBER), USERS_EXECUTING (NUMBER), LOADS (NUMBER), FIRST_LOAD_TIME (VARCHAR2), INVALIDATIONS (NUMBER), PARSE_CALLS (NUMBER), DISK_READS (NUMBER), DIRECT_WRITES (NUMBER), BUFFER_GETS (NUMBER), APPLICATION_WAIT_TIME (NUMBER), CONCURRENCY_WAIT_TIME (NUMBER), CLUSTER_WAIT_TIME (NUMBER), USER_IO_WAIT_TIME (NUMBER), PLSQL_EXEC_TIME (NUMBER), JAVA_EXEC_TIME (NUMBER), ROWS_PROCESSED (NUMBER), COMMAND_TYPE (NUMBER), OPTIMIZER_MODE (VARCHAR2), OPTIMIZER_COST (NUMBER), OPTIMIZER_ENV (RAW), OPTIMIZER_ENV_HASH_VALUE (NUMBER), PARSING_USER_ID (NUMBER), PARSING_SCHEMA_ID (NUMBER), PARSING_SCHEMA_NAME (VARCHAR2), KEPT_VERSIONS (NUMBER), ADDRESS (RAW), HASH_VALUE (NUMBER), OLD_HASH_VALUE (NUMBER), PLAN_HASH_VALUE (NUMBER), MODULE (VARCHAR2), MODULE_HASH (NUMBER), ACTION (VARCHAR2), ACTION_HASH (NUMBER), SERIALIZABLE_ABORTS (NUMBER), OUTLINE_CATEGORY (VARCHAR2), CPU_TIME (NUMBER), ELAPSED_TIME (NUMBER), OUTLINE_SID (VARCHAR2), LAST_ACTIVE_CHILD_ADDRESS (RAW), REMOTE (VARCHAR2), OBJECT_STATUS (VARCHAR2), LITERAL_HASH_VALUE (NUMBER), LAST_LOAD_TIME (DATE), IS_OBSOLETE (VARCHAR2), IS_BIND_SENSITIVE (VARCHAR2), IS_BIND_AWARE (VARCHAR2), CHILD_LATCH (NUMBER), SQL_PROFILE (VARCHAR2), SQL_PATCH (VARCHAR2), SQL_PLAN_BASELINE (VARCHAR2), PROGRAM_ID (NUMBER), PROGRAM_LINE# (NUMBER), EXACT_MATCHING_SIGNATURE (NUMBER), FORCE_MATCHING_SIGNATURE (NUMBER), LAST_ACTIVE_TIME (DATE), BIND_DATA (RAW), TYPECHECK_MEM (NUMBER), IO_CELL_OFFLOAD_ELIGIBLE_BYTES (NUMBER), IO_INTERCONNECT_BYTES (NUMBER), PHYSICAL_READ_REQUESTS (NUMBER), PHYSICAL_READ_BYTES (NUMBER), PHYSICAL_WRITE_REQUESTS (NUMBER), PHYSICAL_WRITE_BYTES (NUMBER), OPTIMIZED_PHY_READ_REQUESTS (NUMBER), LOCKED_TOTAL (NUMBER), PINNED_TOTAL (NUMBER), IO_CELL_UNCOMPRESSED_BYTES (NUMBER), IO_CELL_OFFLOAD_RETURNED_BYTES (NUMBER)

**SQL-статистика (v$sqlarea)**: 10086 строк

### Топ SQL по общее время выполнения (ELAPSED_TIME)
| # | SQL_ID | ELAPSED_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | f32ypq7snu4ft | 807313205670 | 182391 | 786910732337 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 9cxb4zarwdfwb | 508321742983 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | bxz2429wvynkg | 204101338043 | 50668 | 195687040253 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | c4m70z4f1p3xk | 155437393604 | 35521939 | 9868265195 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:19 | SCHEMA=SYS | `SELECT DATABASE_ROLE FROM V$DATABASE` |
| 5 | ch18qpm3dzvs4 | 143613397145 | 425994 | 141683396085 | MODULE=None | PROG=352032:435 | SCHEMA=CCM | `SELECT * FROM (SELECT TO_CHAR(T.PARENT_ID) AS MUN_ID, T.WEIGHT, CASE WHEN NVL((SELECT R.MUN_ID FROM ` |
| 6 | ah8xc0hc2jrah | 134908009320 | 1290965 | 132958683384 | MODULE=LPC2Enter.exe | ACTION=None | PROG=95446:1097 | SCHEMA=CCM | `SELECT /*+ FIRST_ROWS */ ID FROM V_CUSTOM_END_SHIFT WHERE CUSTOM_END_SHIFT = (SELECT MAX(CUSTOM_END_` |
| 7 | 6yqacscpfh5b1 | 129678030908 | 428285 | 127609642926 | MODULE=None | PROG=352032:413 | SCHEMA=CCM | `SELECT COUNT(*) FROM (SELECT T.CLUST_ID FROM TRANS_WEIGHT T WHERE (T.CHILD_ID = :B3 OR T.CHILD_ID = ` |
| 8 | fbm4dy92h8yrp | 77701521754 | 226965 | 76507200415 | MODULE=oracle@p2ano7db1 | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT "A1"."ID3","A1"."HEAT_NO","A1"."LOT_NO","A1"."COIL_NO","A1"."GRADE","A1"."THICKNESS","A1"."WI` |
| 9 | 7hryb0ts6bbyx | 49826526961 | 2453743134 | 48571607695 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=438545:31 | SCHEMA=CCM | `SELECT COUNT(1) FROM MDM_STEEL_MARK SM, MDM_STEEL_MARK_GROUPS SMG WHERE SMG.GROUP_CODE = :B2 AND SM.` |
| 10 | 845hkcdhdg01k | 42814694312 | 1368897253 | 42410860607 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=94725:9 | SCHEMA=CCM | `SELECT M.DIC_GRADE FROM MUNIT M WHERE M.HNUM = (SELECT MM.HNUM FROM MUNIT MM WHERE MM.ID = :B1 ) AND` |
| 11 | 90hcp8y4jdm3p | 36840229439 | 1156 | 36005325515 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 24813 | PROG=0:0 | SCHEMA=CCM | `/*SDS_ID = 24813, CLASS_ID = 903568, TAG_ID = 13081, LOG_USER = polyakov_vv3*/ WITH q AS (     SELEC` |
| 12 | fff9mscf1d7mm | 36086762991 | 302 | 845988924 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 13 | 4gfftp25jqjha | 30622320086 | 7327 | 28587090519 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 14 | 6sjb9q12c5xg0 | 28030113276 | 247485668 | 27027474117 | MODULE=LPC2Former.exe | ACTION=None | PROG=365940:6 | SCHEMA=CCM | `SELECT S.ID FROM SIGN_HEADER SH, SIGN S WHERE SH.ID = S.SH_ID AND SH.CON_FACCESS = 3 AND S.IS_DELETE` |
| 15 | 8zacyugnnv2fx | 22298068013 | 3479363 | 3882521213 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `DECLARE   machinename VARCHAR2(64);   osuserid    VARCHAR2(30);   v_sid       NUMBER(10);   v_serial` |

### Топ SQL по CPU-времени (CPU_TIME)
| # | SQL_ID | CPU_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | f32ypq7snu4ft | 786910732337 | 182391 | 786910732337 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 9cxb4zarwdfwb | 372584141505 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | bxz2429wvynkg | 195687040253 | 50668 | 195687040253 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | ch18qpm3dzvs4 | 141683396085 | 425994 | 141683396085 | MODULE=None | PROG=352032:435 | SCHEMA=CCM | `SELECT * FROM (SELECT TO_CHAR(T.PARENT_ID) AS MUN_ID, T.WEIGHT, CASE WHEN NVL((SELECT R.MUN_ID FROM ` |
| 5 | ah8xc0hc2jrah | 132958683384 | 1290965 | 132958683384 | MODULE=LPC2Enter.exe | ACTION=None | PROG=95446:1097 | SCHEMA=CCM | `SELECT /*+ FIRST_ROWS */ ID FROM V_CUSTOM_END_SHIFT WHERE CUSTOM_END_SHIFT = (SELECT MAX(CUSTOM_END_` |
| 6 | 6yqacscpfh5b1 | 127609642926 | 428285 | 127609642926 | MODULE=None | PROG=352032:413 | SCHEMA=CCM | `SELECT COUNT(*) FROM (SELECT T.CLUST_ID FROM TRANS_WEIGHT T WHERE (T.CHILD_ID = :B3 OR T.CHILD_ID = ` |
| 7 | fbm4dy92h8yrp | 76507200415 | 226965 | 76507200415 | MODULE=oracle@p2ano7db1 | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT "A1"."ID3","A1"."HEAT_NO","A1"."LOT_NO","A1"."COIL_NO","A1"."GRADE","A1"."THICKNESS","A1"."WI` |
| 8 | 7hryb0ts6bbyx | 48571607695 | 2453743134 | 48571607695 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=438545:31 | SCHEMA=CCM | `SELECT COUNT(1) FROM MDM_STEEL_MARK SM, MDM_STEEL_MARK_GROUPS SMG WHERE SMG.GROUP_CODE = :B2 AND SM.` |
| 9 | 845hkcdhdg01k | 42410860607 | 1368897253 | 42410860607 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=94725:9 | SCHEMA=CCM | `SELECT M.DIC_GRADE FROM MUNIT M WHERE M.HNUM = (SELECT MM.HNUM FROM MUNIT MM WHERE MM.ID = :B1 ) AND` |
| 10 | 90hcp8y4jdm3p | 36005325515 | 1156 | 36005325515 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 24813 | PROG=0:0 | SCHEMA=CCM | `/*SDS_ID = 24813, CLASS_ID = 903568, TAG_ID = 13081, LOG_USER = polyakov_vv3*/ WITH q AS (     SELEC` |
| 11 | 4gfftp25jqjha | 28587090519 | 7327 | 28587090519 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 12 | 6sjb9q12c5xg0 | 27027474117 | 247485668 | 27027474117 | MODULE=LPC2Former.exe | ACTION=None | PROG=365940:6 | SCHEMA=CCM | `SELECT S.ID FROM SIGN_HEADER SH, SIGN S WHERE SH.ID = S.SH_ID AND SH.CON_FACCESS = 3 AND S.IS_DELETE` |
| 13 | cr1wz9rfv0yrj | 19774678083 | 78 | 19774678083 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=IGSTSPEP | `SELECT "ID","E_UNIT_ID","NPLV","HNUM","TNUM","NRUL","THICK","WEIGHT","DATE_BORN","IN_GRADE","GRADE",` |
| 14 | fhk3zmb71rruh | 18456259744 | 366513 | 18456259744 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `begin  :v_result := pkglpc2aeipengine.processrecord( :v_in_rollid,:v_in_readdelay,:v_in_numworkreel,` |
| 15 | c5bag3m0uygmq | 18399830770 | 732 | 18399830770 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `merge into PROCESS_TREND_VALUES d using (select :1  as rtime, :2  as base_no, :3  as attr_id, :4  as` |

### Топ SQL по количеству исполнений (EXECUTIONS)
| # | SQL_ID | EXECUTIONS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 7hryb0ts6bbyx | 2453743134 | 2453743134 | 48571607695 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=438545:31 | SCHEMA=CCM | `SELECT COUNT(1) FROM MDM_STEEL_MARK SM, MDM_STEEL_MARK_GROUPS SMG WHERE SMG.GROUP_CODE = :B2 AND SM.` |
| 2 | 42wd7pfmkttf7 | 2017573562 | 2017573562 | 10593643368 | MODULE=None | PROG=277663:729 | SCHEMA=CCM | `SELECT SUBSTR(STR, INSTR(STR, '/', DECODE(LEVEL, 1, 0, 1), GREATEST(LEVEL - 1, 1)) + 1, DECODE(CONNE` |
| 3 | 845hkcdhdg01k | 1368897253 | 1368897253 | 42410860607 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=94725:9 | SCHEMA=CCM | `SELECT M.DIC_GRADE FROM MUNIT M WHERE M.HNUM = (SELECT MM.HNUM FROM MUNIT MM WHERE MM.ID = :B1 ) AND` |
| 4 | c81j9r4zn3ut4 | 514389773 | 514389773 | 2590972130 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=95406:6796 | SCHEMA=CCM | `SELECT MIN(VALUE_INT) FROM TESTS_ANALYSIS WHERE TESTS_ID = :B3 AND KOD_AN = :B2 AND KOD_ATR = :B1 ` |
| 5 | 9cbfxt7q7msgp | 342199162 | 342199162 | 1155903304 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=360558:167 | SCHEMA=CCM | `SELECT MA.ATTRVALUE FROM MDM_DIC_LINE_ATTR MA WHERE MA.MDM_DIC_LINE_ID = :B2 AND MA.ATTRNAMEENG = :B` |
| 6 | 6sjb9q12c5xg0 | 247485668 | 247485668 | 27027474117 | MODULE=LPC2Former.exe | ACTION=None | PROG=365940:6 | SCHEMA=CCM | `SELECT S.ID FROM SIGN_HEADER SH, SIGN S WHERE SH.ID = S.SH_ID AND SH.CON_FACCESS = 3 AND S.IS_DELETE` |
| 7 | 31yj61gg71v2s | 225038497 | 225038497 | 1326369429 | MODULE=None | PROG=95392:113 | SCHEMA=CCM | `SELECT VAL_NUM FROM MUNIT_ATTR WHERE CON_ATTR_ID = :B3 AND MUN_ID = :B2 AND NVL(BUNT_ID, -1) = :B1 ` |
| 8 | 384kcw2vfvh21 | 195188766 | 195188766 | 2875541874 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=396690:7 | SCHEMA=CCM | `SELECT NDIC(DIC_GRADE) FROM MUNIT WHERE ID = :B1 ` |
| 9 | 4z6jfdhm5uxr7 | 174439813 | 174439813 | 1047425317 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SYS | `select longname from javasnm$ where short = :1` |
| 10 | 6p23cay9ndh9r | 147002011 | 147002011 | 499918682 | MODULE=None | PROG=95673:87 | SCHEMA=PI | `SELECT TRUNC(PKG_PRO_UTL_SYS.CALCDATEFROMSHIFTNEW(TRUNC(AH.AKT_DATE), PKG_PRO_UTL_SYS.CALCSHIFTFROMD` |
| 11 | 5xb3ym11ybr47 | 135060810 | 135060810 | 884108703 | MODULE=None | PROG=96589:5 | SCHEMA=PI | `SELECT S_SAP_EVENTS.NEXTVAL FROM DUAL` |
| 12 | 4s71mb2gjbbkt | 110313652 | 110313652 | 1227702251 | MODULE=None | PROG=277663:1014 | SCHEMA=CCM | `SELECT * FROM DOC_INTERCHANGE_ATTR WHERE TAG_ID = :B1 ORDER BY CODE` |
| 13 | 1qsu6rm63k8j8 | 97539863 | 97539863 | 294592598 | MODULE=None | PROG=95570:55 | SCHEMA=CCM | `SELECT NAME FROM CONS WHERE DIC = :B2 AND ID = :B1 ` |
| 14 | bqkjh99nz3f4y | 97086859 | 97086859 | 398501471 | MODULE=None | PROG=95406:6823 | SCHEMA=CCM | `SELECT DECODE(:B1 , 1, 'Уд.', 0, 'Неуд.', -50094, 'O', -50095, 'A', -50096, 'B', -50097, 'C', -50098` |
| 15 | 1v717nvrhgbn9 | 92378767 | 92378767 | 506995251 | MODULE=JDBC Thin Client | ACTION=None | PROG=1311:180 | SCHEMA=TIGER | `SELECT USER FROM SYS.DUAL` |

### Топ SQL по буферным чтениям (BUFFER_GETS)
| # | SQL_ID | BUFFER_GETS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 90hcp8y4jdm3p | 4115193121 | 1156 | 36005325515 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 24813 | PROG=0:0 | SCHEMA=CCM | `/*SDS_ID = 24813, CLASS_ID = 903568, TAG_ID = 13081, LOG_USER = polyakov_vv3*/ WITH q AS (     SELEC` |
| 2 | f32ypq7snu4ft | 4113403526 | 182391 | 786910732337 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | ak03fdj78a9qr | 4025452163 | 61745 | 3769766874 | MODULE=None | PROG=94360:180 | SCHEMA=QLOG | `DELETE FROM QLOG.Q_BUFFER QQQ WHERE QQQ.RULE_ID = :B4 AND (QQQ.PRIME_ID = :B3 OR (QQQ.PRIME_ID IS NU` |
| 4 | bxz2429wvynkg | 3846035682 | 50668 | 195687040253 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | gxzp9542ju15j | 3194760334 | 285 | 1961329238 | MODULE=oracle@nl-sp-bfm-db2 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=BFM | `SELECT /*+ RULE */ "CREATE_DATE","ID","NUM","TYPE","CON_TYPE","TNUM","NRUL","GOST","THICK","DOP_TEST` |
| 6 | 6yqacscpfh5b1 | 3109791871 | 428285 | 127609642926 | MODULE=None | PROG=352032:413 | SCHEMA=CCM | `SELECT COUNT(*) FROM (SELECT T.CLUST_ID FROM TRANS_WEIGHT T WHERE (T.CHILD_ID = :B3 OR T.CHILD_ID = ` |
| 7 | cr1wz9rfv0yrj | 3103299604 | 78 | 19774678083 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=IGSTSPEP | `SELECT "ID","E_UNIT_ID","NPLV","HNUM","TNUM","NRUL","THICK","WEIGHT","DATE_BORN","IN_GRADE","GRADE",` |
| 8 | 3ka01m6wm9nss | 2926968397 | 94737 | 7694738334 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPCCM2 | `SELECT "ATTRID","PRIME_ID","VAL_AVG","VAL_DEV","VAL_MIN","VAL_MAX","VAL_STR","VAL_NUM","VAL_DT","VAL` |
| 9 | 845hkcdhdg01k | 2829295358 | 1368897253 | 42410860607 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=94725:9 | SCHEMA=CCM | `SELECT M.DIC_GRADE FROM MUNIT M WHERE M.HNUM = (SELECT MM.HNUM FROM MUNIT MM WHERE MM.ID = :B1 ) AND` |
| 10 | accb9scsab5w2 | 2816443031 | 166 | 7200871411 | MODULE=DBMS_SCHEDULER | ACTION=PPL_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 11 | fjnkf3n3x4420 | 2712572250 | 41947 | 9500694851 | MODULE=None | PROG=95673:206 | SCHEMA=PI | `SELECT DECODE(V.DIC_STAGE, 69, :B2 , 68, 28, V.DIC_STAGE) FROM (SELECT M.ID, E.DIC_STAGE FROM V_EM_L` |
| 12 | 6vj91ugbwxxnx | 2664869950 | 1397 | 5647546884 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 13 | 9nsnc2sbdfpjs | 2400877530 | 694912 | 1221775845 | MODULE=LPC2Enter.exe | ACTION=None | PROG=95446:2559 | SCHEMA=CCM | `SELECT COLUMN_VALUE AS CLUST_ID FROM TABLE(PKGLPC2ENTER.GETCLUSTIDINSHIFT(:B1 )) ORDER BY COLUMN_VAL` |
| 14 | 7hryb0ts6bbyx | 2340606082 | 2453743134 | 48571607695 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=438545:31 | SCHEMA=CCM | `SELECT COUNT(1) FROM MDM_STEEL_MARK SM, MDM_STEEL_MARK_GROUPS SMG WHERE SMG.GROUP_CODE = :B2 AND SM.` |
| 15 | b155xmhs6d1ha | 2328928506 | 512842 | 3919001275 | MODULE=None | PROG=278805:5 | SCHEMA=PI | `UPDATE SAP_EVENTS SET DATE_PROCESS = SYSDATE WHERE OBJECT_KEY = :B3 AND ID_EVENT <= :B2 AND E_TYPE =` |

### Топ SQL по дисковым чтениям (DISK_READS)
| # | SQL_ID | DISK_READS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | bxz2429wvynkg | 181949117 | 50668 | 195687040253 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 9cxb4zarwdfwb | 171757383 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | fff9mscf1d7mm | 48604683 | 302 | 845988924 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 94g411zjfuj2f | 23012212 | 76 | 398549492 | MODULE=None | PROG=94394:6 | SCHEMA=PI | `SELECT T.ID_EVENT FROM SAP_EVENTS T WHERE T.DATE_PROCESS IS NOT NULL AND T.DATE_EVENT < (SYSDATE - :` |
| 5 | 8fnnuruz1zp4v | 18720274 | 201 | 117973593 | MODULE=MainCommunicator | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `select heat_no, lot_no, coil_no, id3, width, thickness, weight, grade, perm_sign, note from l2common` |
| 6 | 8wrws9b4ak0mb | 14156988 | 54 | 39370821 | MODULE=LPC2Act.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select (select m.width from munit m, V_AEIP_CURRENT_PARENT t where t.con_stage = :STAGE and m.id = t` |
| 7 | c580xnqtyrvc5 | 12592843 | 41 | 290569773 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | ch18qpm3dzvs4 | 10257499 | 425994 | 141683396085 | MODULE=None | PROG=352032:435 | SCHEMA=CCM | `SELECT * FROM (SELECT TO_CHAR(T.PARENT_ID) AS MUN_ID, T.WEIGHT, CASE WHEN NVL((SELECT R.MUN_ID FROM ` |
| 9 | b155xmhs6d1ha | 9036409 | 512842 | 3919001275 | MODULE=None | PROG=278805:5 | SCHEMA=PI | `UPDATE SAP_EVENTS SET DATE_PROCESS = SYSDATE WHERE OBJECT_KEY = :B3 AND ID_EVENT <= :B2 AND E_TYPE =` |
| 10 | f32ypq7snu4ft | 7631043 | 182391 | 786910732337 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 11 | 9a8g441nn0766 | 7055055 | 2399500 | 239772675 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select SQLTEXT from smart_sqltext where SMART_DSET_ID=:ID and SQL_DATE is null order by id ` |
| 12 | 96dajs6yb370p | 5805003 | 22 | 586741024 | MODULE=None | PROG=95525:686 | SCHEMA=PI | `SELECT R.* FROM (SELECT ID FROM PI.V_EM_FROM_IDB MINUS SELECT ID FROM PI.V_EM_FROM_CCM) R WHERE NOT ` |
| 13 | 19ugvuwrghrjr | 5005352 | 142 | 116476240 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 14 | bvkut4s1qprtr | 4832226 | 142 | 167219845 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 15 | fbvk0rqggupxa | 4437083 | 10 | 538188875 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по времени ожидания приложения (APPLICATION_WAIT_TIME)
| # | SQL_ID | APPLICATION_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | cw0ddnbz0np9d | 7058039196 | 100270 | 4349243120 | MODULE=None | PROG=0:0 | SCHEMA=ARPL | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN dbms_` |
| 2 | 5ghj5mg4qsjat | 3479383423 | 1502 | 17027831 | MODULE=None | PROG=0:0 | SCHEMA=ARPL | `UPDATE "ARPL"."SHAPKA" SET "GDIS" = :1,"KCEH" = :2,"NSNZ" = :3,"DPID" = :4,"NLIM" = :5,"KDOR" = :6,"` |
| 3 | 1gnq8wmk4w02f | 1832611074 | 51370 | 22998178 | MODULE=None | PROG=0:0 | SCHEMA=ARPL | `SELECT /*+ remote_mapped("ARPL.NLMK") */ CURRENT$."GDIS",CURRENT$."KCEH",CURRENT$."NSNZ",CURRENT$."D` |
| 4 | 1f39h5hp4wrqf | 721848679 | 51370 | 12018510 | MODULE=None | PROG=0:0 | SCHEMA=ARPL | `SELECT /*+ remote_mapped("ARPL.NLMK") */ CURRENT$."GDIS",CURRENT$."NSNZ",CURRENT$."DPID",CURRENT$."N` |
| 5 | accb9scsab5w2 | 580506289 | 166 | 7200871411 | MODULE=DBMS_SCHEDULER | ACTION=PPL_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 6 | 0hgn3skbp048n | 240319543 | 414 | 693675 | MODULE=None | PROG=0:0 | SCHEMA=ARPL | `UPDATE "ARPL"."SHAPKAEX" SET "GDIS" = :1,"NSNZ" = :2,"DPID" = :3,"NLIM" = :4,"KDSN" = :5,"NZCH" = :6` |
| 7 | dk02k5bzfq29m | 173803843 | 134 | 1701271049 | MODULE=DBMS_SCHEDULER | ACTION=MGO4_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 8 | fhk3zmb71rruh | 162183665 | 366513 | 18456259744 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `begin  :v_result := pkglpc2aeipengine.processrecord( :v_in_rollid,:v_in_readdelay,:v_in_numworkreel,` |
| 9 | 0m6ng066ksbk4 | 159010862 | 128 | 984179079 | MODULE=DBMS_SCHEDULER | ACTION=AEIP6_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 10 | 7b46rb1gupr3n | 144873837 | 12773 | 252232159 | MODULE=LPC2Sign.exe | ACTION=None | PROG=0:0 | SCHEMA=SUMAEV_MA | `begin  sys.dbms_alert.waitany(:name, :message, :status); end; ` |

### Топ SQL по времени ожидания конкурентности (CONCURRENCY_WAIT_TIME)
| # | SQL_ID | CONCURRENCY_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | ankyttqyhf43t | 518848586 | 8010092 | 3421199915 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=TIGER | `DECLARE   uu VARCHAR2(100);   hh VARCHAR2(200);   xhh VARCHAR2(200) := '-';   i NUMBER := 0;   j NUM` |
| 2 | accb9scsab5w2 | 278110998 | 166 | 7200871411 | MODULE=DBMS_SCHEDULER | ACTION=PPL_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 3 | 9cxb4zarwdfwb | 210508582 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | cayxucwsgbm10 | 188437536 | 144 | 2168680719 | MODULE=DBMS_SCHEDULER | ACTION=RVS_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 5 | 8h7dd8c2hn04s | 187833741 | 24015 | 1908115557 | MODULE=RVS_PROC | ACTION=Event Processing | PROG=95615:561 | SCHEMA=L2COMMON | `begin l2common.prvs_proc.processCoilData; end;` |
| 6 | 8zacyugnnv2fx | 179381600 | 3479363 | 3882521213 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `DECLARE   machinename VARCHAR2(64);   osuserid    VARCHAR2(30);   v_sid       NUMBER(10);   v_serial` |
| 7 | 453k5817faygx | 160697712 | 3582773 | 346149482 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:25 | SCHEMA=SYS | `INSERT INTO DB_MANAGER.LOGONAUDITTABLE VALUES ( 'LOGON', :B6 , :B5 , :B4 , SYSDATE, USER, :B3 , :B2 ` |
| 8 | bxz2429wvynkg | 126540934 | 50668 | 195687040253 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 9 | 4vs91dcv7u1p6 | 122046475 | 6360651 | 914068008 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `insert into sys.aud$( sessionid,entryid,statement,ntimestamp#, userid,userhost,terminal,action#,retu` |
| 10 | fk9vwag6bt63f | 103025596 | 31201156 | 14794794626 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:11 | SCHEMA=SYS | `SELECT S.SID, S.SERIAL#, P.SPID, S.OSUSER, S.MACHINE, S.PROGRAM FROM V$SESSION S, V$PROCESS P WHERE ` |

### Топ SQL по времени ожидания кластера (CLUSTER_WAIT_TIME)
| # | SQL_ID | CLUSTER_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 9c27pfjpmw00d | 0 | 38673 | 4902111 | MODULE=None | PROG=97232:13 | SCHEMA=CCM | `INSERT INTO LOG_MELT (LOG_ID, LOG_CALLSTACK, SH2, SPB, CON_DOMAIN, SZN, SMG, IS_EXPERIMENTAL, NPLV, ` |
| 2 | 8ar84kdyss01u | 0 | 34 | 532043765 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 18712 | PROG=0:0 | SCHEMA=CCM | `/*SDS_ID = 18712, CLASS_ID = 14241, TAG_ID = 14241, LOG_USER = */ --TFastDataSet with q as  (select ` |
| 3 | bmuz08fa0h023 | 0 | 24 | 1113 | MODULE=LPC2Utk.exe | ACTION=None | PROG=95402:778 | SCHEMA=CCM | `SELECT FH.FORMNUM, ER.TREATDATE, FH.CON_ORDERKIND, FH.IS_SHIP_CONTAINER FROM FORMHEADER FH, ENTER_RE` |
| 4 | 877zx4yb90023 | 0 | 486 | 134625 | MODULE=None | PROG=270686:707 | SCHEMA=L2COMMON | `MERGE INTO MEASUREMENTS D USING (SELECT :B7 AS MUNIT_CID, :B6 AS ATTR_ID, :B5 AS STAGE_ID, :B4 AS ID` |
| 5 | 1gnq8wmk4w02f | 0 | 51370 | 22998178 | MODULE=None | PROG=0:0 | SCHEMA=ARPL | `SELECT /*+ remote_mapped("ARPL.NLMK") */ CURRENT$."GDIS",CURRENT$."KCEH",CURRENT$."NSNZ",CURRENT$."D` |
| 6 | 2w7928yk4w02m | 0 | 280 | 7001 | MODULE=LPC2Former.exe | ACTION=None | PROG=95406:8933 | SCHEMA=CCM | `SELECT M.CON_STAGE, ER.DIC_STAGE FROM MUNIT M, ENTER_REQUIS ER WHERE M.ENT_ID = ER.ID AND M.ID = :B1` |
| 7 | 1fkh93md0802n | 0 | 0 | 911 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select   LOW_OPTIMAL_SIZE,           HIGH_OPTIMAL_SIZE,           OPTIMAL_EXECUTIONS,           ONEP` |
| 8 | 1cmdjtbs5w02r | 0 | 18 | 58911 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `insert into P_CUT_DEFECT  (CON_DEFECT, CON_LOCATION, CON_CULPRIT, MUN_ID, WEIGHT_DEFECT, DEF_SCRIPT,` |
| 9 | bjkphvsh6n049 | 0 | 11 | 16229 | MODULE=LPC2Act.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select ndic(t.DIC_FIO1) L_DIC_FIO1,ndic(t.DIC_FIO2) L_DIC_FIO2,ndic(t.DIC_FIO3) L_DIC_FIO3,ncon(t.CO` |
| 10 | 10wm16xg0004b | 0 | 11 | 23725 | MODULE=LPC2Act.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `update DEF_AEIP_BUF t set t.POS_DEATH = :POS_DEATH, t.DATE_DEATH = SysDate where /*sav_1*/ t.munit_i` |

### Топ SQL по времени ожидания пользовательского IO (USER_IO_WAIT_TIME)
| # | SQL_ID | USER_IO_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 9cxb4zarwdfwb | 128175558582 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | fff9mscf1d7mm | 35521441630 | 302 | 845988924 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | 94g411zjfuj2f | 12627596478 | 76 | 398549492 | MODULE=None | PROG=94394:6 | SCHEMA=PI | `SELECT T.ID_EVENT FROM SAP_EVENTS T WHERE T.DATE_PROCESS IS NOT NULL AND T.DATE_EVENT < (SYSDATE - :` |
| 4 | c4m70z4f1p3xk | 10003585214 | 35521939 | 9868265195 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:19 | SCHEMA=SYS | `SELECT DATABASE_ROLE FROM V$DATABASE` |
| 5 | c580xnqtyrvc5 | 6959845364 | 41 | 290569773 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | f32ypq7snu4ft | 6314043968 | 182391 | 786910732337 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | b155xmhs6d1ha | 6015470916 | 512842 | 3919001275 | MODULE=None | PROG=278805:5 | SCHEMA=PI | `UPDATE SAP_EVENTS SET DATE_PROCESS = SYSDATE WHERE OBJECT_KEY = :B3 AND ID_EVENT <= :B2 AND E_TYPE =` |
| 8 | 9a8g441nn0766 | 5281739913 | 2399500 | 239772675 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select SQLTEXT from smart_sqltext where SMART_DSET_ID=:ID and SQL_DATE is null order by id ` |
| 9 | c81j9r4zn3ut4 | 4119255474 | 514389773 | 2590972130 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=95406:6796 | SCHEMA=CCM | `SELECT MIN(VALUE_INT) FROM TESTS_ANALYSIS WHERE TESTS_ID = :B3 AND KOD_AN = :B2 AND KOD_ATR = :B1 ` |
| 10 | bxz2429wvynkg | 3736009209 | 50668 | 195687040253 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по физическим чтениям в байтах (PHYSICAL_READ_BYTES)
| # | SQL_ID | PHYSICAL_READ_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | c4m70z4f1p3xk | 4074021838848 | 35521939 | 9868265195 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:19 | SCHEMA=SYS | `SELECT DATABASE_ROLE FROM V$DATABASE` |
| 2 | bxz2429wvynkg | 1490527166464 | 50668 | 195687040253 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | 9cxb4zarwdfwb | 1407036481536 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | dkg56jfbs49pt | 821194661888 | 103987 | 1365167112 | MODULE=zabbix_proxy: odbc poller #8 [got 4 values in 0. | ACTION=None | PROG=0:0 | SCHEMA=ZABBIX | `SELECT  tablespace_name AS tablespace,  contents FROM DBA_TABLESPACES` |
| 5 | 8zacyugnnv2fx | 400638115840 | 3479363 | 3882521213 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `DECLARE   machinename VARCHAR2(64);   osuserid    VARCHAR2(30);   v_sid       NUMBER(10);   v_serial` |
| 6 | fff9mscf1d7mm | 398169563136 | 302 | 845988924 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 94g411zjfuj2f | 188516040704 | 76 | 398549492 | MODULE=None | PROG=94394:6 | SCHEMA=PI | `SELECT T.ID_EVENT FROM SAP_EVENTS T WHERE T.DATE_PROCESS IS NOT NULL AND T.DATE_EVENT < (SYSDATE - :` |
| 8 | 8fnnuruz1zp4v | 153356484608 | 201 | 117973593 | MODULE=MainCommunicator | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `select heat_no, lot_no, coil_no, id3, width, thickness, weight, grade, perm_sign, note from l2common` |
| 9 | 2qr52n47vt2aj | 138319126528 | 1249 | 1442945172 | MODULE=emagent_SQL_oracle_database | ACTION=ha_backup_sbt | PROG=0:0 | SCHEMA=DBSNMP | `WITH jd AS (SELECT end_time, input_bytes, session_key, session_recid, session_stamp, input_type, out` |
| 10 | 8wrws9b4ak0mb | 115974045696 | 54 | 39370821 | MODULE=LPC2Act.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select (select m.width from munit m, V_AEIP_CURRENT_PARENT t where t.con_stage = :STAGE and m.id = t` |

### Топ SQL по физическим записям в байтах (PHYSICAL_WRITE_BYTES)
| # | SQL_ID | PHYSICAL_WRITE_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 9cxb4zarwdfwb | 1828454400 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 9pj66awucd7tn | 1467228160 | 14140 | 3939944856 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | 4221h6mdb3xt1 | 1077952512 | 103988 | 8568281313 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | accb9scsab5w2 | 898277376 | 166 | 7200871411 | MODULE=DBMS_SCHEDULER | ACTION=PPL_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 5 | dk02k5bzfq29m | 641564672 | 134 | 1701271049 | MODULE=DBMS_SCHEDULER | ACTION=MGO4_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 6 | 2qr52n47vt2aj | 554188800 | 1249 | 1442945172 | MODULE=emagent_SQL_oracle_database | ACTION=ha_backup_sbt | PROG=0:0 | SCHEMA=DBSNMP | `WITH jd AS (SELECT end_time, input_bytes, session_key, session_recid, session_stamp, input_type, out` |
| 7 | 4gfftp25jqjha | 537231360 | 7327 | 28587090519 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | fhk3zmb71rruh | 521527296 | 366513 | 18456259744 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `begin  :v_result := pkglpc2aeipengine.processrecord( :v_in_rollid,:v_in_readdelay,:v_in_numworkreel,` |
| 9 | f32ypq7snu4ft | 451837952 | 182391 | 786910732337 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 10 | fu1t4fm6yy18y | 407248896 | 87353 | 19971518 | MODULE=LPC2Enter.exe | ACTION=None | PROG=394529:1207 | SCHEMA=CCM | `WITH T AS (SELECT TL.ID, TL.DATE_TEST FROM TESTS_LABEL_DS TL, TESTS_TRANS_DS TT, TESTS_CARD_DS TC WH` |

### Топ SQL по байтам IO через interconnect (IO_INTERCONNECT_BYTES)
| # | SQL_ID | IO_INTERCONNECT_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | c4m70z4f1p3xk | 4074031013888 | 35521939 | 9868265195 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:19 | SCHEMA=SYS | `SELECT DATABASE_ROLE FROM V$DATABASE` |
| 2 | bxz2429wvynkg | 1490686951424 | 50668 | 195687040253 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | 9cxb4zarwdfwb | 1408864935936 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | dkg56jfbs49pt | 821194661888 | 103987 | 1365167112 | MODULE=zabbix_proxy: odbc poller #8 [got 4 values in 0. | ACTION=None | PROG=0:0 | SCHEMA=ZABBIX | `SELECT  tablespace_name AS tablespace,  contents FROM DBA_TABLESPACES` |
| 5 | 8zacyugnnv2fx | 400638230528 | 3479363 | 3882521213 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `DECLARE   machinename VARCHAR2(64);   osuserid    VARCHAR2(30);   v_sid       NUMBER(10);   v_serial` |
| 6 | fff9mscf1d7mm | 398169563136 | 302 | 845988924 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 94g411zjfuj2f | 188516040704 | 76 | 398549492 | MODULE=None | PROG=94394:6 | SCHEMA=PI | `SELECT T.ID_EVENT FROM SAP_EVENTS T WHERE T.DATE_PROCESS IS NOT NULL AND T.DATE_EVENT < (SYSDATE - :` |
| 8 | 8fnnuruz1zp4v | 153356484608 | 201 | 117973593 | MODULE=MainCommunicator | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `select heat_no, lot_no, coil_no, id3, width, thickness, weight, grade, perm_sign, note from l2common` |
| 9 | 2qr52n47vt2aj | 138873315328 | 1249 | 1442945172 | MODULE=emagent_SQL_oracle_database | ACTION=ha_backup_sbt | PROG=0:0 | SCHEMA=DBSNMP | `WITH jd AS (SELECT end_time, input_bytes, session_key, session_recid, session_stamp, input_type, out` |
| 10 | 8wrws9b4ak0mb | 115974045696 | 54 | 39370821 | MODULE=LPC2Act.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select (select m.width from munit m, V_AEIP_CURRENT_PARENT t where t.con_stage = :STAGE and m.id = t` |

### Топ SQL по байтам, подходящим под offload (IO_CELL_OFFLOAD_ELIGIBLE_BYTES)
| # | SQL_ID | IO_CELL_OFFLOAD_ELIGIBLE_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 9c27pfjpmw00d | 0 | 38673 | 4902111 | MODULE=None | PROG=97232:13 | SCHEMA=CCM | `INSERT INTO LOG_MELT (LOG_ID, LOG_CALLSTACK, SH2, SPB, CON_DOMAIN, SZN, SMG, IS_EXPERIMENTAL, NPLV, ` |
| 2 | 8ar84kdyss01u | 0 | 34 | 532043765 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 18712 | PROG=0:0 | SCHEMA=CCM | `/*SDS_ID = 18712, CLASS_ID = 14241, TAG_ID = 14241, LOG_USER = */ --TFastDataSet with q as  (select ` |
| 3 | bmuz08fa0h023 | 0 | 24 | 1113 | MODULE=LPC2Utk.exe | ACTION=None | PROG=95402:778 | SCHEMA=CCM | `SELECT FH.FORMNUM, ER.TREATDATE, FH.CON_ORDERKIND, FH.IS_SHIP_CONTAINER FROM FORMHEADER FH, ENTER_RE` |
| 4 | 877zx4yb90023 | 0 | 486 | 134625 | MODULE=None | PROG=270686:707 | SCHEMA=L2COMMON | `MERGE INTO MEASUREMENTS D USING (SELECT :B7 AS MUNIT_CID, :B6 AS ATTR_ID, :B5 AS STAGE_ID, :B4 AS ID` |
| 5 | 1gnq8wmk4w02f | 0 | 51370 | 22998178 | MODULE=None | PROG=0:0 | SCHEMA=ARPL | `SELECT /*+ remote_mapped("ARPL.NLMK") */ CURRENT$."GDIS",CURRENT$."KCEH",CURRENT$."NSNZ",CURRENT$."D` |
| 6 | 2w7928yk4w02m | 0 | 280 | 7001 | MODULE=LPC2Former.exe | ACTION=None | PROG=95406:8933 | SCHEMA=CCM | `SELECT M.CON_STAGE, ER.DIC_STAGE FROM MUNIT M, ENTER_REQUIS ER WHERE M.ENT_ID = ER.ID AND M.ID = :B1` |
| 7 | 1fkh93md0802n | 0 | 0 | 911 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select   LOW_OPTIMAL_SIZE,           HIGH_OPTIMAL_SIZE,           OPTIMAL_EXECUTIONS,           ONEP` |
| 8 | 1cmdjtbs5w02r | 0 | 18 | 58911 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `insert into P_CUT_DEFECT  (CON_DEFECT, CON_LOCATION, CON_CULPRIT, MUN_ID, WEIGHT_DEFECT, DEF_SCRIPT,` |
| 9 | bjkphvsh6n049 | 0 | 11 | 16229 | MODULE=LPC2Act.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select ndic(t.DIC_FIO1) L_DIC_FIO1,ndic(t.DIC_FIO2) L_DIC_FIO2,ndic(t.DIC_FIO3) L_DIC_FIO3,ncon(t.CO` |
| 10 | 10wm16xg0004b | 0 | 11 | 23725 | MODULE=LPC2Act.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `update DEF_AEIP_BUF t set t.POS_DEATH = :POS_DEATH, t.DATE_DEATH = SysDate where /*sav_1*/ t.munit_i` |

### Топ SQL по числу физических чтений (PHYSICAL_READ_REQUESTS)
| # | SQL_ID | PHYSICAL_READ_REQUESTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | c4m70z4f1p3xk | 248658556 | 35521939 | 9868265195 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:19 | SCHEMA=SYS | `SELECT DATABASE_ROLE FROM V$DATABASE` |
| 2 | 9cxb4zarwdfwb | 161553373 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | dkg56jfbs49pt | 50121754 | 103987 | 1365167112 | MODULE=zabbix_proxy: odbc poller #8 [got 4 values in 0. | ACTION=None | PROG=0:0 | SCHEMA=ZABBIX | `SELECT  tablespace_name AS tablespace,  contents FROM DBA_TABLESPACES` |
| 4 | fff9mscf1d7mm | 48604467 | 302 | 845988924 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | 8zacyugnnv2fx | 24550493 | 3479363 | 3882521213 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `DECLARE   machinename VARCHAR2(64);   osuserid    VARCHAR2(30);   v_sid       NUMBER(10);   v_serial` |
| 6 | 94g411zjfuj2f | 23012212 | 76 | 398549492 | MODULE=None | PROG=94394:6 | SCHEMA=PI | `SELECT T.ID_EVENT FROM SAP_EVENTS T WHERE T.DATE_PROCESS IS NOT NULL AND T.DATE_EVENT < (SYSDATE - :` |
| 7 | c580xnqtyrvc5 | 12592843 | 41 | 290569773 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | b155xmhs6d1ha | 9036409 | 512842 | 3919001275 | MODULE=None | PROG=278805:5 | SCHEMA=PI | `UPDATE SAP_EVENTS SET DATE_PROCESS = SYSDATE WHERE OBJECT_KEY = :B3 AND ID_EVENT <= :B2 AND E_TYPE =` |
| 9 | 9a8g441nn0766 | 7054874 | 2399500 | 239772675 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select SQLTEXT from smart_sqltext where SMART_DSET_ID=:ID and SQL_DATE is null order by id ` |
| 10 | f32ypq7snu4ft | 6439432 | 182391 | 786910732337 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по числу физических записей (PHYSICAL_WRITE_REQUESTS)
| # | SQL_ID | PHYSICAL_WRITE_REQUESTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 9cxb4zarwdfwb | 219187 | 21458 | 372584141505 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 9pj66awucd7tn | 179105 | 14140 | 3939944856 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | 4221h6mdb3xt1 | 128528 | 103988 | 8568281313 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | accb9scsab5w2 | 109653 | 166 | 7200871411 | MODULE=DBMS_SCHEDULER | ACTION=PPL_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 5 | dk02k5bzfq29m | 78316 | 134 | 1701271049 | MODULE=DBMS_SCHEDULER | ACTION=MGO4_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 6 | 4gfftp25jqjha | 64050 | 7327 | 28587090519 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | fhk3zmb71rruh | 63663 | 366513 | 18456259744 | MODULE=LPC2Enter.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `begin  :v_result := pkglpc2aeipengine.processrecord( :v_in_rollid,:v_in_readdelay,:v_in_numworkreel,` |
| 8 | fu1t4fm6yy18y | 49713 | 87353 | 19971518 | MODULE=LPC2Enter.exe | ACTION=None | PROG=394529:1207 | SCHEMA=CCM | `WITH T AS (SELECT TL.ID, TL.DATE_TEST FROM TESTS_LABEL_DS TL, TESTS_TRANS_DS TT, TESTS_CARD_DS TC WH` |
| 9 | cayxucwsgbm10 | 44160 | 144 | 2168680719 | MODULE=DBMS_SCHEDULER | ACTION=RVS_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 10 | 8h7dd8c2hn04s | 43853 | 24015 | 1908115557 | MODULE=RVS_PROC | ACTION=Event Processing | PROG=95615:561 | SCHEMA=L2COMMON | `begin l2common.prvs_proc.processCoilData; end;` |

### Топ SQL по обработанным строкам (ROWS_PROCESSED)
| # | SQL_ID | ROWS_PROCESSED | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 7hryb0ts6bbyx | 2453708360 | 2453743134 | 48571607695 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=438545:31 | SCHEMA=CCM | `SELECT COUNT(1) FROM MDM_STEEL_MARK SM, MDM_STEEL_MARK_GROUPS SMG WHERE SMG.GROUP_CODE = :B2 AND SM.` |
| 2 | 42wd7pfmkttf7 | 2420661818 | 2017573562 | 10593643368 | MODULE=None | PROG=277663:729 | SCHEMA=CCM | `SELECT SUBSTR(STR, INSTR(STR, '/', DECODE(LEVEL, 1, 0, 1), GREATEST(LEVEL - 1, 1)) + 1, DECODE(CONNE` |
| 3 | 845hkcdhdg01k | 1327979348 | 1368897253 | 42410860607 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=94725:9 | SCHEMA=CCM | `SELECT M.DIC_GRADE FROM MUNIT M WHERE M.HNUM = (SELECT MM.HNUM FROM MUNIT MM WHERE MM.ID = :B1 ) AND` |
| 4 | 71jmmngj8dfmb | 997975661 | 10620134 | 2693928261 | MODULE=None | PROG=95613:271 | SCHEMA=TM | `SELECT T.* FROM TM_T_TASKS T, TM_THR_GROUPS TG WHERE T.GROUP_ID = TG.GROUP_ID AND (T.COUNT_EXEC = 0 ` |
| 5 | 4s71mb2gjbbkt | 585728435 | 110313652 | 1227702251 | MODULE=None | PROG=277663:1014 | SCHEMA=CCM | `SELECT * FROM DOC_INTERCHANGE_ATTR WHERE TAG_ID = :B1 ORDER BY CODE` |
| 6 | c81j9r4zn3ut4 | 514388972 | 514389773 | 2590972130 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=95406:6796 | SCHEMA=CCM | `SELECT MIN(VALUE_INT) FROM TESTS_ANALYSIS WHERE TESTS_ID = :B3 AND KOD_AN = :B2 AND KOD_ATR = :B1 ` |
| 7 | 384kcw2vfvh21 | 195188566 | 195188766 | 2875541874 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=396690:7 | SCHEMA=CCM | `SELECT NDIC(DIC_GRADE) FROM MUNIT WHERE ID = :B1 ` |
| 8 | 4z6jfdhm5uxr7 | 169999551 | 174439813 | 1047425317 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SYS | `select longname from javasnm$ where short = :1` |
| 9 | cm5vu20fhtnq1 | 160801337 | 32109857 | 966053030 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select /*+ connect_by_filtering */ privilege#,level from sysauth$ connect by grantee#=prior privileg` |
| 10 | 5xb3ym11ybr47 | 135060822 | 135060810 | 884108703 | MODULE=None | PROG=96589:5 | SCHEMA=PI | `SELECT S_SAP_EVENTS.NEXTVAL FROM DUAL` |

### Топ SQL по сортировкам (SORTS)
| # | SQL_ID | SORTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 845hkcdhdg01k | 1368896083 | 1368897253 | 42410860607 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=94725:9 | SCHEMA=CCM | `SELECT M.DIC_GRADE FROM MUNIT M WHERE M.HNUM = (SELECT MM.HNUM FROM MUNIT MM WHERE MM.ID = :B1 ) AND` |
| 2 | 4s71mb2gjbbkt | 110313670 | 110313652 | 1227702251 | MODULE=None | PROG=277663:1014 | SCHEMA=CCM | `SELECT * FROM DOC_INTERCHANGE_ATTR WHERE TAG_ID = :B1 ORDER BY CODE` |
| 3 | 6sjb9q12c5xg0 | 101581079 | 247485668 | 27027474117 | MODULE=LPC2Former.exe | ACTION=None | PROG=365940:6 | SCHEMA=CCM | `SELECT S.ID FROM SIGN_HEADER SH, SIGN S WHERE SH.ID = S.SH_ID AND SH.CON_FACCESS = 3 AND S.IS_DELETE` |
| 4 | bq667fpuubc79 | 44403485 | 44403484 | 656917436 | MODULE=None | PROG=352032:1028 | SCHEMA=CCM | `SELECT T.TESTS_ID, T.KOD_AN, T.KOD_ATR FROM TESTS_ANALYSIS T WHERE T.TESTS_ID = :B2 AND T.KOD_AN || ` |
| 5 | c4m70z4f1p3xk | 35523070 | 35521939 | 9868265195 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:19 | SCHEMA=SYS | `SELECT DATABASE_ROLE FROM V$DATABASE` |
| 6 | 0ws7ahf1d78qa | 19524698 | 9760394 | 1190859954 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select SYS_CONTEXT('USERENV', 'SERVER_HOST'), SYS_CONTEXT('USERENV', 'DB_UNIQUE_NAME'), SYS_CONTEXT(` |
| 7 | 788j5pfgj8jcw | 11980581 | 11980563 | 608330099 | MODULE=None | PROG=94723:9 | SCHEMA=CCM | `SELECT NDIC(M.DIC_GRADE) FROM MUNIT M WHERE M.HNUM = (SELECT MM.HNUM FROM MUNIT MM WHERE MM.ID = :B1` |
| 8 | 71jmmngj8dfmb | 10620134 | 10620134 | 2693928261 | MODULE=None | PROG=95613:271 | SCHEMA=TM | `SELECT T.* FROM TM_T_TASKS T, TM_THR_GROUPS TG WHERE T.GROUP_ID = TG.GROUP_ID AND (T.COUNT_EXEC = 0 ` |
| 9 | cd3yq6pcqx0p1 | 9191180 | 4595581 | 161972891 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select  count(*), max(scn)   from   (select sp.BO#, sp.PMOPTYPE,           case bitand(sp.flags, 8) ` |
| 10 | 13qbhm5kcphj6 | 8088140 | 8088140 | 83043352 | MODULE=None | PROG=352032:1158 | SCHEMA=CCM | `SELECT CASE WHEN A.KOD_AN IN (1162, 1967, 2051 ) THEN PKGMAGN_TESTING.GETSTRVALUE(A.VALUE_INT) ELSE ` |

### Топ SQL по числу парсингов (PARSE_CALLS)
| # | SQL_ID | PARSE_CALLS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | a7jgtb8v0qhyp | 44504993 | 45113202 | 293483842 | MODULE=None | PROG=95406:1983 | SCHEMA=CCM | `SELECT MAX(BAN) FROM P_CUT WHERE MUN_ID = :B1 ` |
| 2 | cm5vu20fhtnq1 | 32124989 | 32109857 | 966053030 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select /*+ connect_by_filtering */ privilege#,level from sysauth$ connect by grantee#=prior privileg` |
| 3 | c4m70z4f1p3xk | 31529659 | 35521939 | 9868265195 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:19 | SCHEMA=SYS | `SELECT DATABASE_ROLE FROM V$DATABASE` |
| 4 | fk9vwag6bt63f | 31203131 | 31201156 | 14794794626 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:11 | SCHEMA=SYS | `SELECT S.SID, S.SERIAL#, P.SPID, S.OSUSER, S.MACHINE, S.PROGRAM FROM V$SESSION S, V$PROCESS P WHERE ` |
| 5 | 1v717nvrhgbn9 | 18207768 | 92378767 | 506995251 | MODULE=JDBC Thin Client | ACTION=None | PROG=1311:180 | SCHEMA=TIGER | `SELECT USER FROM SYS.DUAL` |
| 6 | 7hryb0ts6bbyx | 16247636 | 2453743134 | 48571607695 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 6882 | PROG=438545:31 | SCHEMA=CCM | `SELECT COUNT(1) FROM MDM_STEEL_MARK SM, MDM_STEEL_MARK_GROUPS SMG WHERE SMG.GROUP_CODE = :B2 AND SM.` |
| 7 | 40u8tx5u21c2x | 15959113 | 15960096 | 2486203900 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `declare   l_program_name varchar2(50 char); --Имя программы begin   /*   sav 04.07.2018 6000010506  ` |
| 8 | 62b998tkua5rk | 15956898 | 15957946 | 1868604677 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:14 | SCHEMA=SYS | `SELECT UPPER(S.PROGRAM) FROM SYS.V_$SESSION S WHERE S.AUDSID = USERENV('sessionid') AND ROWNUM = 1` |
| 9 | bqkjh99nz3f4y | 15108658 | 97086859 | 398501471 | MODULE=None | PROG=95406:6823 | SCHEMA=CCM | `SELECT DECODE(:B1 , 1, 'Уд.', 0, 'Неуд.', -50094, 'O', -50095, 'A', -50096, 'B', -50097, 'C', -50098` |
| 10 | 89w8y2pgn25yd | 13797367 | 13797367 | 185145535 | MODULE=zabbix_proxy: odbc poller #8 [got 0 values in 0. | ACTION=None | PROG=0:0 | SCHEMA=SYS | `select ts# from ts$ where ts$.online$ != 3 and bitand(flags,2048) != 2048` |

### Топ SQL по занимаемой памяти в пуле (SHARABLE_MEM)
| # | SQL_ID | SHARABLE_MEM | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 3ka01m6wm9nss | 102542793 | 94737 | 7694738334 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPCCM2 | `SELECT "ATTRID","PRIME_ID","VAL_AVG","VAL_DEV","VAL_MIN","VAL_MAX","VAL_STR","VAL_NUM","VAL_DT","VAL` |
| 2 | gdfnamzvn5rr6 | 60590857 | 0 | 34041 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPCCM2 | `SELECT /*+ FULL(P) +*/ * FROM "CCM"."VSUP_INT" P` |
| 3 | 87d90h3swfdn4 | 38552838 | 20594 | 19103559 | MODULE=None | PROG=95596:413 | SCHEMA=CCM | `SELECT MAX(SC), MAX(SMN), MAX(SSI), MAX(SP), MAX(SS), MAX(SCR), MAX(SNI), MAX(SCU), MAX(SAL), MAX(SN` |
| 4 | 9jk5afyzdrcht | 23867413 | 111 | 2689858 | MODULE=SQL*Plus | ACTION=None | PROG=12314:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+rule*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('M_VIEW_T', '7')), KU$.MVIEW_TAB.OBJ` |
| 5 | ggu8xspju1vvh | 20640441 | 81 | 2964891 | MODULE=SQL*Plus | ACTION=None | PROG=12314:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+all_rows*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('TABLE_T', '7')), KU$.OBJ_NUM FR` |
| 6 | ac4w2ys8hgpsb | 18380453 | 11 | 96521806 | MODULE=LPC2Utk.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select ncon(t.CON_FACCESS,54) L_CON_FACCESS,ncon(t.CON_ORDERKIND,60) L_CON_ORDERKIND,ndic(t.DIC_GOST` |
| 7 | 1ufbt9za4rvq0 | 18167823 | 23 | 116732815 | MODULE=LPC2Utk.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select ncon(t.CON_FACCESS,54) L_CON_FACCESS,ncon(t.CON_ORDERKIND,60) L_CON_ORDERKIND,ndic(t.DIC_GOST` |
| 8 | 4k2sfrsvgh3p0 | 18081708 | 0 | 638359 | MODULE=LPC2Utk.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select rownum nn, fin.*   from (select f.*           from (select distinct t2.nplv,                 ` |
| 9 | 7xb4axhqds1fb | 9534224 | 3308 | 388883968 | MODULE=zabbix_proxy: odbc poller #4 [got 1 values in 0. | ACTION=None | PROG=0:0 | SCHEMA=ZABBIX | `SELECT 'SYS::' || METRIC_NAME AS METRIC, ROUND(VALUE,3) as VALUE   FROM GV$SYSMETRIC WHERE GROUP_ID ` |
| 10 | 16w0mmawynn5p | 8414177 | 2194 | 4432188 | MODULE=LPC2Former.exe | ACTION=None | PROG=95393:611 | SCHEMA=CCM | `SELECT /*+ FIRST_ROWS  OPTIMIZER_FEATURES_ENABLE('9.2.0')  */ COUNT(*) FROM MUNIT M, ENTER_REQUIS E ` |

### Топ SQL по числу версий (VERSION_COUNT)
| # | SQL_ID | VERSION_COUNT | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 87d90h3swfdn4 | 537 | 20594 | 19103559 | MODULE=None | PROG=95596:413 | SCHEMA=CCM | `SELECT MAX(SC), MAX(SMN), MAX(SSI), MAX(SP), MAX(SS), MAX(SCR), MAX(SNI), MAX(SCU), MAX(SAL), MAX(SN` |
| 2 | 16w0mmawynn5p | 476 | 2194 | 4432188 | MODULE=LPC2Former.exe | ACTION=None | PROG=95393:611 | SCHEMA=CCM | `SELECT /*+ FIRST_ROWS  OPTIMIZER_FEATURES_ENABLE('9.2.0')  */ COUNT(*) FROM MUNIT M, ENTER_REQUIS E ` |
| 3 | fyw4ww9a30hvr | 454 | 0 | 2753 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 4 | 7ng34ruy5awxq | 302 | 5723 | 713304 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select i.obj#,i.ts#,i.file#,i.block#,i.intcols,i.type#,i.flags,i.property,i.pctfree$,i.initrans,i.ma` |
| 5 | bcf392ajzntdy | 234 | 0 | 24318 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 6 | 4m7m0t6fjcs5x | 165 | 7140943 | 302202194 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `update seq$ set increment$=:2,minvalue=:3,maxvalue=:4,cycle#=:5,order$=:6,cache=:7,highwater=:8,audi` |
| 7 | db78fxqxwxt7r | 161 | 185134 | 9045327 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select /*+ rule */ bucket, endpoint, col#, epvalue from histgrm$ where obj#=:1 and intcol#=:2 and ro` |
| 8 | f711myt0q6cma | 159 | 102824 | 6713490 | MODULE=WebDSet.exe | ACTION=None | PROG=0:0 | SCHEMA=SYS | `insert into sys.aud$( sessionid,entryid,statement,ntimestamp#, userid,userhost,terminal,action#,retu` |
| 9 | 6rhbg92ks2gzq | 154 | 0 | 17573 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 10 | 1gu8t96d0bdmu | 147 | 64370 | 2226281 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select t.ts#,t.file#,t.block#,nvl(t.bobj#,0),nvl(t.tab#,0),t.intcols,nvl(t.clucols,0),t.audit$,t.fla` |

### Нагрузка по источнику (MODULE)
| MODULE | SQL | Executions | CPU (ms) | Elapsed (ms) | Buffer | Disk | RowsProc |
|--------|----:|-----------:|---------:|-------------:|-------:|-----:|--------:|
| None | 2428 | 3852727910 | 1839355189 | 2195562795 | 53200752766 | 528548708 | 5654001908 |
| LPC2Enter.exe | 1798 | 200239825 | 204911264 | 223480362 | 15089973353 | 25077327 | 277998725 |
| NLMKReport.exe | 643 | 3790028355 | 105221356 | 113427928 | 18245893738 | 10191332 | 3493823762 |
| oracle@nl-sp-mes-db3 (TNS V1-V3) | 208 | 1599723589 | 89398643 | 93298385 | 14203372138 | 3290720 | 1767911171 |
| oracle@p2ano7db1 | 1 | 226965 | 76507200 | 77701521 | 870143873 | 240 | 0 |
| JDBC Thin Client | 114 | 224417702 | 58525189 | 226061877 | 2391169144 | 1199773 | 247149812 |
| LPC2Former.exe | 479 | 513507966 | 31997857 | 38248280 | 4352029878 | 6970386 | 349915702 |
| oracle@p2ano7db1 (TNS V1-V3) | 79 | 42893 | 14262497 | 14566750 | 2598635906 | 327499 | 107 |
| DBMS_SCHEDULER | 1275 | 202651 | 13794832 | 22552666 | 3506235076 | 9897189 | 299073 |
| LPC2MagnTesting.exe | 235 | 18937850 | 5897745 | 7177821 | 1192148414 | 2971855 | 17358276 |
| oracle@nl-sp-qidb-db1 (TNS V1-V3) | 3 | 222642 | 5744380 | 6958616 | 315057962 | 1749145 | 0 |
| smallgate.exe | 13 | 2364920 | 5265666 | 5753629 | 192629607 | 278689 | 2364920 |
| LPC2Act.exe | 1163 | 10594919 | 4969231 | 7708794 | 1310033132 | 23875548 | 16232238 |
| oracle@nl-sp-bfm-db2 (TNS V1-V3) | 13 | 3473 | 2828712 | 3593496 | 3885184642 | 4869017 | 4351 |
| emagent_SQL_oracle_database | 85 | 886873 | 2132953 | 4395905 | 21241520 | 3700053 | 1119220 |
| RVS_PROC | 9 | 185305 | 1918616 | 3314125 | 223582744 | 3135504 | 162471 |
| zabbix_proxy: odbc poller #8 [got 4 values in 0. | 1 | 103987 | 1365167 | 14915075 | 21421324 | 21 | 4159480 |
| PPL_PROC | 37 | 911530 | 1309308 | 1530707 | 530065160 | 131689 | 685704 |
| LPC2NovellGateSGV.exe | 11 | 7297971 | 847584 | 1290745 | 215301411 | 387803 | 7258822 |
| sgr.exe | 2 | 2033830 | 792065 | 1272368 | 23842102 | 106801 | 2033830 |

### Нагрузка по схеме парсинга (PARSING_SCHEMA_NAME)
| SCHEMA | SQL | Executions | CPU (ms) | Elapsed (ms) | Buffer | Disk |
|--------|----:|-----------:|---------:|-------------:|-------:|-----:|
| TM | 225 | 22062182 | 1436703328 | 1700708494 | 22494503922 | 454482096 |
| CCM | 5874 | 9262630405 | 725583866 | 785615955 | 59689693771 | 93194171 |
| L2COMMON | 293 | 5002013 | 130900005 | 141339426 | 10173873139 | 32386291 |
| SYS | 2505 | 495236421 | 62353248 | 241099889 | 1652175344 | 5148116 |
| IGSTSPEP | 40 | 3773 | 36772527 | 38094661 | 6556067209 | 2245298 |
| PI | 173 | 325924342 | 20034398 | 40662171 | 7883612854 | 43328644 |
| QLOG | 13 | 23402343 | 18040148 | 19224911 | 6321339716 | 2005003 |
| ARPL | 110 | 8881741 | 13011254 | 52947871 | 780749699 | 518862 |
| SUPCCM2 | 89 | 1222585 | 7833026 | 8616948 | 3083995477 | 94125 |
| LINK_QIDB | 3 | 222642 | 5744380 | 6958616 | 315057962 | 1749145 |
| TIGER | 6 | 102782248 | 4074494 | 5069357 | 97315612 | 18646 |
| ZABBIX | 16 | 1980507 | 2854388 | 17981793 | 40741989 | 77960 |
| BFM | 11 | 1286 | 2828452 | 3592809 | 3885156205 | 4868818 |
| DBSNMP | 152 | 2358362 | 2235043 | 4518706 | 21554327 | 3759132 |
| DB2KAFKA | 9 | 303726 | 1521592 | 1753276 | 85181266 | 57151 |

### Нагрузка по типу команды (COMMAND_TYPE)
| COMMAND_TYPE | SQL | Executions | CPU (ms) |
|--------------|----:|-----------:|---------:|
| 47 | 635 | 48210946 | 1500992912 |
| 3 | 7346 | 10182703360 | 923027541 |
| 189 | 26 | 705720 | 18468454 |
| 2 | 581 | 22569928 | 17384073 |
| 6 | 1072 | 39387150 | 7248730 |
| 7 | 313 | 11063470 | 4390310 |
| 170 | 9 | 1912 | 962281 |
| 90 | 1 | 1291105 | 30799 |
| 26 | 54 | 25515 | 541 |
| 1 | 49 | 49 | 147 |

### Агрегаты по пулу
- Всего SQL: **10086**
- Суммарно исполнений: **10305959155**
- CPU: **2472505792 ms**
- Время выполнения: **3073835096 ms**
- Буферные чтения: **123558732122**
- Дисковые чтения: **648679790**
- Сортировки: **1804052041**
- Парсинги (PARSE_CALLS): **769782290**
- Обработанные строки (ROWS_PROCESSED): **11968001156**
- Занимаемая память пула (SHARABLE_MEM): **1.2 GB**

### SQL с высоким VERSION_COUNT (>20) — кандидаты на проблему версионирования
| SQL_ID | VersionCount | Источник | SQL |
|--------|-------------:|----------|-----|
| 2n61mg0ya06d1 | 24 | MODULE=LPC2Act.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select rowid, ID,DIC,NAME,INVISIBLE from DICS where DIC=:P_DIC and GRP is null and DIC_ID is null an` |
| fsbqktj5vw6n9 | 30 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select next_run_date, obj#, run_job, sch_job from (select decode(bitand(a.flags, 16384), 0, a.next_r` |
| fyw4ww9a30hvr | 454 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 47r1y8yn34jmj | 23 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select default$ from col$ where rowid=:1` |
| 2syvqzbxp4k9z | 60 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select u.name, o.name, a.interface_version#, o.obj#      from association$ a, user$ u, obj$ o       ` |
| 16w0mmawynn5p | 476 | MODULE=LPC2Former.exe | ACTION=None | PROG=95393:611 | SCHEMA=CCM | `SELECT /*+ FIRST_ROWS  OPTIMIZER_FEATURES_ENABLE('9.2.0')  */ COUNT(*) FROM MUNIT M, ENTER_REQUIS E ` |
| 4m7m0t6fjcs5x | 165 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `update seq$ set increment$=:2,minvalue=:3,maxvalue=:4,cycle#=:5,order$=:6,cache=:7,highwater=:8,audi` |
| bcf392ajzntdy | 234 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| dha5nv6urcumf | 38 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select binaryDefVal,           lengthb(binaryDefVal) from ecol$ where tabobj# = :1 and colnum = :2` |
| bb9gqvmdm0wt3 | 24 | MODULE=LPC2Enter.exe | ACTION=None | PROG=97665:1319 | SCHEMA=CCM | `SELECT NVL(MAX(TT.YEAR), TO_CHAR(SYSDATE, 'yy')) FROM MELT TT WHERE TT.NPLV = :B1` |

### SQL с инвалидациями (INVALIDATIONS>0)
| SQL_ID | Invalidations | Источник | SQL |
|--------|--------------:|----------|-----|
| 3jdvpdq81h05d | 2 | MODULE=LPC2Enter.exe | ACTION=None | PROG=95446:262 | SCHEMA=CCM | `INSERT INTO TRANS T (MUN_ID, ID) VALUES (:B2 , :B1 )` |
| 0t565p2x34069 | 18 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 24813 | PROG=290458:769 | SCHEMA=CCM | `SELECT SUM(T.WEIGHT) AS SUM_WEIGHT FROM CCM.V_LPC2STATE_PRODUCTION T WHERE (T.TREATDATE >= :B4 ) AND` |
| f06snrsa800fy | 2 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `LOCK TABLE "SYS"."ORA_TEMP_1_DS_450132" IN EXCLUSIVE MODE  NOWAIT ` |
| 4dy1xm4nxc0gf | 4 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `insert into wrh$_system_event   (snap_id, dbid, instance_number, event_id,    total_waits, total_tim` |
| b8k3wgfubn0gu | 11 | MODULE=LPC2Sign.exe | ACTION=None | PROG=0:0 | SCHEMA=CCM | `select ndic((select dic_grade from munit where id = d.mun_id)) L_MDIC_GRADE,d.ID,d.LENGTH,(select di` |
| 92vtt9m0r80jb | 2 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=IGSTSPEP | `SELECT /*+ FULL(P) +*/ * FROM "V_SUKP_ANALIZ_DES_WATER_CHL" P` |
| f25c6z3gw40ks | 1 | MODULE=DBMS_SCHEDULER | ACTION=ORA$AT_OS_OPT_SY_22586 | PROG=12727:19269 | SCHEMA=SYS | `select substrb(dump(val,16,0,32),1,120) ep, cnt from (select /*+ no_expand_table(t) index_rs(t)   no` |
| 6axxaq3tsc0rf | 4 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `WITH a as (SELECT a$.dbid, a$.snap_id, a$.INSTANCE_NUMBER, a$.SQL_ID, a$.event_id FROM  WRH$_ACTIVE_` |
| cc3t5rx6q40wp | 2 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `LOCK TABLE "SYS"."ORA_TEMP_1_DS_450127" IN EXCLUSIVE MODE  NOWAIT ` |
| ct4bn4dt8s17m | 2 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `LOCK TABLE "SYS"."ORA_TEMP_1_DS_450139" IN EXCLUSIVE MODE  NOWAIT ` |

---

## ccm2 / table_sizes

_Файл_: `./spider-output/ccm2_table_sizes.json`, строк: 135

_Колонки_: SEGMENT_NAME (VARCHAR2), SEGMENT_TYPE (VARCHAR2), BYTES (NUMBER)

**Сегменты**: 135 шт, суммарно **5.2 GB**

### Топ сегментов по размеру
| # | SEGMENT_NAME | SEGMENT_TYPE | BYTES |
|---|--------------|--------------|------:|
| 1 | OUTCARTS_LOG | TABLE PARTITION | 1140850688 (1.1 GB) |
| 2 | MEASUREMENTS_PK | INDEX | 545259520 (520.0 MB) |
| 3 | BROCKHAUS_TREND_VALUES | TABLE | 469762048 (448.0 MB) |
| 4 | BROCKHAUS_TREND_VALUES_PK | INDEX | 452984832 (432.0 MB) |
| 5 | MEASUREMENTS | TABLE PARTITION | 335544320 (320.0 MB) |
| 6 | FRAGMENTS_LOG | TABLE PARTITION | 276824064 (264.0 MB) |
| 7 | INCARTS_LOG | TABLE PARTITION | 243269632 (232.0 MB) |
| 8 | STRIPS_LOG_STRIP_CID_FK | INDEX | 234881024 (224.0 MB) |
| 9 | STRIPS_LOG | TABLE PARTITION | 218103808 (208.0 MB) |
| 10 | FRAGMENTS_LOG_FRAGMENT_CID_FK | INDEX | 209715200 (200.0 MB) |
| 11 | OUTCARTS_LOG_OUTCART_CID_FK | INDEX | 184549376 (176.0 MB) |
| 12 | FRAGMENTS_LOG_PK | INDEX | 142606336 (136.0 MB) |
| 13 | OUTCARTS | TABLE PARTITION | 134217728 (128.0 MB) |
| 14 | OUTCARTS_LOG_PK | INDEX | 117440512 (112.0 MB) |
| 15 | STRIPS_LOG_PK | INDEX | 117440512 (112.0 MB) |
| 16 | INCARTS_LOG_INCART_CID_FK | INDEX | 92274688 (88.0 MB) |
| 17 | INCARTS | TABLE PARTITION | 83886080 (80.0 MB) |
| 18 | FRAGMENTS_STRIP_ID_FK | INDEX | 49283072 (47.0 MB) |
| 19 | INCARTS_LOG_PK | INDEX | 49283072 (47.0 MB) |
| 20 | STRIPS_PARENT_STRIP_ID_FK | INDEX | 46137344 (44.0 MB) |

### Размер по типу сегмента
| SEGMENT_TYPE | Суммарно |
|--------------|---------:|
| INDEX | 2.4 GB |
| TABLE PARTITION | 2.3 GB |
| TABLE | 510.7 MB |
| LOB PARTITION | 16.0 MB |
| LOBSEGMENT | 4.1 MB |
| LOBINDEX | 960.0 KB |
| INDEX PARTITION | 256.0 KB |

---

## ccm2 / tablespaces

_Файл_: `./spider-output/ccm2_tablespaces.json`, строк: 40

_Колонки_: TABLESPACE_NAME (VARCHAR2), BYTES (NUMBER)

**Табличные пространства**: 40 шт

| NAME | TYPE | SIZE | FREE | USED % |
|------|------|-----:|-----:|-------:|
| CCMTABL |  | 212.8 GB | 212.8 GB | 0.0% |
| UNDOTBS1 |  | 74.8 GB | 74.8 GB | 0.0% |
| L2LTC2TABL |  | 73.1 GB | 73.1 GB | 0.0% |
| CCMINDX |  | 57.6 GB | 57.6 GB | 0.0% |
| L2LTC2INDX |  | 55.2 GB | 55.2 GB | 0.0% |
| PIINDX |  | 29.6 GB | 29.6 GB | 0.0% |
| USERS |  | 29.2 GB | 29.2 GB | 0.0% |
| QLOGTABL |  | 15.6 GB | 15.6 GB | 0.0% |
| L2MGO4TABL |  | 14.5 GB | 14.5 GB | 0.0% |
| CCMLOG |  | 12.9 GB | 12.9 GB | 0.0% |
| L2LTCTABL |  | 8.9 GB | 8.9 GB | 0.0% |
| L2LTCINDX |  | 6.6 GB | 6.6 GB | 0.0% |
| L2AEIP6T |  | 6.2 GB | 6.2 GB | 0.0% |
| PITABL |  | 5.6 GB | 5.6 GB | 0.0% |
| L2TABL |  | 5.5 GB | 5.5 GB | 0.0% |
| L2RVS |  | 5.1 GB | 5.1 GB | 0.0% |
| L2RM1770TABL |  | 4.5 GB | 4.5 GB | 0.0% |
| SYSAUX |  | 3.3 GB | 3.3 GB | 0.0% |
| SYSTEM |  | 2.7 GB | 2.7 GB | 0.0% |
| ETLTABL |  | 2.0 GB | 2.0 GB | 0.0% |
| PLANINDX |  | 1.6 GB | 1.6 GB | 0.0% |
| TMTABL |  | 1.5 GB | 1.5 GB | 0.0% |
| ARPLTABL |  | 1.4 GB | 1.4 GB | 0.0% |
| INDX |  | 1.2 GB | 1.2 GB | 0.0% |
| L2PPLTABL |  | 950.0 MB | 950.0 MB | 0.0% |
| PLANTABL |  | 700.0 MB | 700.0 MB | 0.0% |
| SUPQUE |  | 650.0 MB | 650.0 MB | 0.0% |
| L2LOITABL |  | 500.0 MB | 500.0 MB | 0.0% |
| L2AEIP6I |  | 450.0 MB | 450.0 MB | 0.0% |
| SERTTABL |  | 250.0 MB | 250.0 MB | 0.0% |
| L2KAFKATABL |  | 200.0 MB | 200.0 MB | 0.0% |
| DB2KAFKATABL |  | 180.0 MB | 180.0 MB | 0.0% |
| TDTABL |  | 100.0 MB | 100.0 MB | 0.0% |
| XDB |  | 100.0 MB | 100.0 MB | 0.0% |
| L2QUE |  | 100.0 MB | 100.0 MB | 0.0% |
| ODM |  | 100.0 MB | 100.0 MB | 0.0% |
| SERTINDX |  | 100.0 MB | 100.0 MB | 0.0% |
| ARPLINDX |  | 100.0 MB | 100.0 MB | 0.0% |
| TMINDX |  | 50.0 MB | 50.0 MB | 0.0% |
| TEMP |  | 0.0 B | 0.0 B | 0.0% |

---

## ccm3t / table_sizes

_Файл_: `./spider-output/ccm3t_table_sizes.json`, строк: 0

_Колонки_: SEGMENT_NAME (VARCHAR2), SEGMENT_TYPE (VARCHAR2), BYTES (NUMBER)

**Сегменты**: 0 шт, суммарно **0.0 B**

### Топ сегментов по размеру
| # | SEGMENT_NAME | SEGMENT_TYPE | BYTES |
|---|--------------|--------------|------:|

---

## ccm4 / shared_pool

_Файл_: `./spider-output/ccm4_shared_pool.json`, строк: 63832

_Колонки_: SQL_TEXT (VARCHAR2), SQL_ID (VARCHAR2), SHARABLE_MEM (NUMBER), PERSISTENT_MEM (NUMBER), RUNTIME_MEM (NUMBER), SORTS (NUMBER), VERSION_COUNT (NUMBER), LOADED_VERSIONS (NUMBER), OPEN_VERSIONS (NUMBER), USERS_OPENING (NUMBER), FETCHES (NUMBER), EXECUTIONS (NUMBER), PX_SERVERS_EXECUTIONS (NUMBER), END_OF_FETCH_COUNT (NUMBER), USERS_EXECUTING (NUMBER), LOADS (NUMBER), FIRST_LOAD_TIME (VARCHAR2), INVALIDATIONS (NUMBER), PARSE_CALLS (NUMBER), DISK_READS (NUMBER), DIRECT_WRITES (NUMBER), BUFFER_GETS (NUMBER), APPLICATION_WAIT_TIME (NUMBER), CONCURRENCY_WAIT_TIME (NUMBER), CLUSTER_WAIT_TIME (NUMBER), USER_IO_WAIT_TIME (NUMBER), PLSQL_EXEC_TIME (NUMBER), JAVA_EXEC_TIME (NUMBER), ROWS_PROCESSED (NUMBER), COMMAND_TYPE (NUMBER), OPTIMIZER_MODE (VARCHAR2), OPTIMIZER_COST (NUMBER), OPTIMIZER_ENV (RAW), OPTIMIZER_ENV_HASH_VALUE (NUMBER), PARSING_USER_ID (NUMBER), PARSING_SCHEMA_ID (NUMBER), PARSING_SCHEMA_NAME (VARCHAR2), KEPT_VERSIONS (NUMBER), ADDRESS (RAW), HASH_VALUE (NUMBER), OLD_HASH_VALUE (NUMBER), PLAN_HASH_VALUE (NUMBER), MODULE (VARCHAR2), MODULE_HASH (NUMBER), ACTION (VARCHAR2), ACTION_HASH (NUMBER), SERIALIZABLE_ABORTS (NUMBER), OUTLINE_CATEGORY (VARCHAR2), CPU_TIME (NUMBER), ELAPSED_TIME (NUMBER), OUTLINE_SID (VARCHAR2), LAST_ACTIVE_CHILD_ADDRESS (RAW), REMOTE (VARCHAR2), OBJECT_STATUS (VARCHAR2), LITERAL_HASH_VALUE (NUMBER), LAST_LOAD_TIME (DATE), IS_OBSOLETE (VARCHAR2), IS_BIND_SENSITIVE (VARCHAR2), IS_BIND_AWARE (VARCHAR2), CHILD_LATCH (NUMBER), SQL_PROFILE (VARCHAR2), SQL_PATCH (VARCHAR2), SQL_PLAN_BASELINE (VARCHAR2), PROGRAM_ID (NUMBER), PROGRAM_LINE# (NUMBER), EXACT_MATCHING_SIGNATURE (NUMBER), FORCE_MATCHING_SIGNATURE (NUMBER), LAST_ACTIVE_TIME (DATE), BIND_DATA (RAW), TYPECHECK_MEM (NUMBER), IO_CELL_OFFLOAD_ELIGIBLE_BYTES (NUMBER), IO_INTERCONNECT_BYTES (NUMBER), PHYSICAL_READ_REQUESTS (NUMBER), PHYSICAL_READ_BYTES (NUMBER), PHYSICAL_WRITE_REQUESTS (NUMBER), PHYSICAL_WRITE_BYTES (NUMBER), OPTIMIZED_PHY_READ_REQUESTS (NUMBER), LOCKED_TOTAL (NUMBER), PINNED_TOTAL (NUMBER), IO_CELL_UNCOMPRESSED_BYTES (NUMBER), IO_CELL_OFFLOAD_RETURNED_BYTES (NUMBER)

**SQL-статистика (v$sqlarea)**: 63832 строк

### Топ SQL по общее время выполнения (ELAPSED_TIME)
| # | SQL_ID | ELAPSED_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 7b46rb1gupr3n | 2578359221224 | 61935071 | 36512598562 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PUHOV_ON | `begin  sys.dbms_alert.waitany(:name, :message, :status); end; ` |
| 2 | 828vhjjc42pb2 | 2395120178885 | 27041 | 1615919227068 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | ap2ac5fhbudky | 987522014786 | 168409 | 782821550409 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 1gygbnwk3hru5 | 924039287950 | 25671 | 587041190180 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | 1gf9w13xds7tt | 575628155244 | 22887 | 77033824755 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | gz6zuj1aqrv4s | 527281128967 | 15625523 | 515950324072 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=20898530:5 | SCHEMA=PPA | `SELECT CASE WHEN T.KONC_CR_3 IS NOT NULL THEN 'Cr 3+' WHEN T.KONC_CR_6 IS NOT NULL THEN 'Cr 6+' ELSE` |
| 7 | 22xa3aws6wpgf | 477509661822 | 2278298 | 458235795333 | MODULE=None | PROG=73612:2746 | SCHEMA=PPA | `SELECT MAX(NCON(T.CON_TYPE_PASSIVE, 462)) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS I` |
| 8 | 18p9x7wr540uk | 324602730423 | 1748 | 242781070902 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 9 | gf2bcjz9uxkf6 | 310264619528 | 3537 | 1074959347 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 10 | gaag4a9n8ksj4 | 297263537665 | 135169 | 144492005754 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 11 | cqqw679y723qj | 264050389889 | 43013 | 1905292673 | MODULE=LPPUtk.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   PkgForm_Utk_ud.StadiaForm(:p1, :p2); end; ` |
| 12 | 31f3bgx0twa7d | 254331220499 | 1129 | 5691509181 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 13 | 1rzm7xsy69uzv | 212766914469 | 133 | 2057474680 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 14 | 7p76ara3hshjn | 211934408742 | 29370 | 1579550341 | MODULE=ANGC1_PROC | ACTION=Event Processing | PROG=4247273:527 | SCHEMA=L2COMMON | `begin l2common.pangc1_proc.processOutputCoilState; end;` |
| 15 | 8r31d4ss7ydrh | 178565258531 | 17854 | 418148209 | MODULE=LPPUtk.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   PkgForm_Utk_ud.set_lock(:p1, :p2); end; ` |

### Топ SQL по CPU-времени (CPU_TIME)
| # | SQL_ID | CPU_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 828vhjjc42pb2 | 1615919227068 | 27041 | 1615919227068 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | ap2ac5fhbudky | 782821550409 | 168409 | 782821550409 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | 1gygbnwk3hru5 | 587041190180 | 25671 | 587041190180 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | gz6zuj1aqrv4s | 515950324072 | 15625523 | 515950324072 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=20898530:5 | SCHEMA=PPA | `SELECT CASE WHEN T.KONC_CR_3 IS NOT NULL THEN 'Cr 3+' WHEN T.KONC_CR_6 IS NOT NULL THEN 'Cr 6+' ELSE` |
| 5 | 22xa3aws6wpgf | 458235795333 | 2278298 | 458235795333 | MODULE=None | PROG=73612:2746 | SCHEMA=PPA | `SELECT MAX(NCON(T.CON_TYPE_PASSIVE, 462)) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS I` |
| 6 | 18p9x7wr540uk | 242781070902 | 1748 | 242781070902 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | gaag4a9n8ksj4 | 144492005754 | 135169 | 144492005754 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | 9pj66awucd7tn | 95343215871 | 259891 | 95343215871 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 9 | 025upv02gy137 | 85395176951 | 435813251 | 85395176951 | MODULE=LPPFormer.exe | ACTION=None | PROG=73612:2929 | SCHEMA=PPA | `SELECT CONTAINERTYPETOP FROM CRISPERNUM_INFO WHERE :B1 LIKE CRISPERNUM || '%'` |
| 10 | 1gf9w13xds7tt | 77033824755 | 22887 | 77033824755 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 11 | 2nf7hytyjdu8p | 76336595845 | 40459 | 76336595845 | MODULE=EnterAkt.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   :doa__value := PkgEnterAKT.GetNEW_NUMAKT_SITE(:p1); end; ` |
| 12 | 2cx3wa3vyjsqz | 67606349732 | 257786 | 67606349732 | MODULE=LPPFormer.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   PkgFormier_ud.AddComToForm(:p1, :p2, :p3, :p4); end; ` |
| 13 | f47p0xwj0k5ju | 48234528672 | 222168 | 48234528672 | MODULE=None | PROG=28475896:442 | SCHEMA=PPA | `SELECT A.* ,CASE WHEN A.CODE IN (399 ) THEN 2 ELSE 1 END AS TYPE_VALUE ,CASE WHEN A.CODE = 310 THEN ` |
| 14 | 5yuyukc07zkwj | 48073252994 | 207199 | 48073252994 | MODULE=LPPFormer.exe | ACTION=None | PROG=73612:2761 | SCHEMA=PPA | `SELECT MAX(T.CON_PASSIVE) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS ID FROM TABLE(CAS` |
| 15 | 26v6uw1vtnsyt | 45100925063 | 3618 | 45100925063 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по количеству исполнений (EXECUTIONS)
| # | SQL_ID | EXECUTIONS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0y0bshnsvqkb8 | 3835993641 | 3835993641 | 26336368123 | MODULE=LPPUtk.exe | ACTION=None | PROG=73529:19 | SCHEMA=PPA | `SELECT PARENT_ID FROM V_TRANS T WHERE CHILD_ID = :B1 ` |
| 2 | bs0xu4cq8b64h | 3167995965 | 3167995965 | 5339738220 | MODULE=None | PROG=75093:1448 | SCHEMA=VOLCANO1_BASE | `SELECT NVL(:B2 , ID3_COM) AS V_ID3 FROM L2COMMON.BASKET WHERE CID2 = :B1 ` |
| 3 | 3ugjrunwsncwz | 1433069139 | 1433069139 | 5194723159 | MODULE=None | PROG=71681:146 | SCHEMA=OBJEVENT | `select object_id, last_event from EVENT_params where last_event = (select min (last_event) from EVEN` |
| 4 | 42wd7pfmkttf7 | 1350486666 | 1350486666 | 29617419957 | MODULE=None | PROG=21576064:729 | SCHEMA=PPA | `SELECT SUBSTR(STR, INSTR(STR, '/', DECODE(LEVEL, 1, 0, 1), GREATEST(LEVEL - 1, 1)) + 1, DECODE(CONNE` |
| 5 | grv7jrnzvt0xt | 1304822470 | 1304822470 | 28168318691 | MODULE=LPPFormer.exe | ACTION=None | PROG=3281742:128 | SCHEMA=PPA | `SELECT * FROM COM_EXT_ATTR T WHERE T.CON_ATTR_ID = :B3 AND T.COM_ID = :B2 AND NVL(T.BUNT_ID, -1) = N` |
| 6 | cmprrupqvkx7t | 1167648694 | 1167648694 | 2729725658 | MODULE=PHPPAgregate.exe | ACTION=None | PROG=3916106:7 | SCHEMA=PPA | `SELECT COUNT(1) FROM PACK_COM_DISCARD T WHERE T.COM_ID = :B1 AND T.ENT_ID_CANCEL IS NULL AND ROWNUM ` |
| 7 | 54bcnvmggt692 | 703499217 | 703499217 | 1722204176 | MODULE=None | PROG=466592:630 | SCHEMA=VOLCANO1_BASE | `SELECT A.CON_COVERING FROM PPA.APROLL A WHERE A.ID = :B1 ` |
| 8 | 05z0ax4bv0f71 | 682554264 | 682554264 | 4734391945 | MODULE=LPPUtk.exe | ACTION=None | PROG=73529:10 | SCHEMA=PPA | `SELECT CHILD_ID FROM V_TRANS T WHERE PARENT_ID = :B1 ` |
| 9 | 4z6jfdhm5uxr7 | 554573559 | 554573559 | 2695187768 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `select longname from javasnm$ where short = :1` |
| 10 | 025upv02gy137 | 435813251 | 435813251 | 85395176951 | MODULE=LPPFormer.exe | ACTION=None | PROG=73612:2929 | SCHEMA=PPA | `SELECT CONTAINERTYPETOP FROM CRISPERNUM_INFO WHERE :B1 LIKE CRISPERNUM || '%'` |
| 11 | 7p5zz1yntsnxk | 387489663 | 387489663 | 2402846993 | MODULE=None | PROG=71681:85 | SCHEMA=OBJEVENT | `select EVENT_SEQ.NEXTVAL from dual` |
| 12 | dtmncf0h7dgba | 324461171 | 324461171 | 2310796199 | MODULE=LPPFormer.exe | ACTION=None | PROG=73516:97 | SCHEMA=PPA | `SELECT NAME FROM CONS WHERE DIC = :B2 AND ID = :B1 AND DATE_END IS NULL AND ROWNUM = 1 ` |
| 13 | 70788s65jdw51 | 297091004 | 297091004 | 1806458931 | MODULE=LPPFormer.exe | ACTION=None | PROG=3281742:114 | SCHEMA=PPA | `SELECT COM_ID, BUNT_ID FROM (SELECT ID AS COM_ID, NULL AS BUNT_ID FROM COM WHERE ID = :B1 UNION ALL ` |
| 14 | dtgkan0c7sw0q | 262939281 | 262939281 | 2345541041 | MODULE=LPPFormer.exe | ACTION=None | PROG=3201802:7 | SCHEMA=VOLCANO1_BASE | `SELECT M.MACHINE_NAME FROM MACHINES M WHERE M.DIC_STAGE = :B2 AND NVL(M.ASSEMBLY,0) = NVL(:B1 ,0)` |
| 15 | 3dg0j9n55b71w | 259457483 | 259457483 | 2838854024 | MODULE=None | PROG=73582:93 | SCHEMA=PPA | `SELECT * FROM PPA.COM WHERE ID = :B1 ` |

### Топ SQL по буферным чтениям (BUFFER_GETS)
| # | SQL_ID | BUFFER_GETS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | ak03fdj78a9qr | 4095212003 | 666136 | 2498331388 | MODULE=None | PROG=6373684:158 | SCHEMA=QLOG | `DELETE FROM QLOG.Q_BUFFER QQQ WHERE QQQ.RULE_ID = :B4 AND (QQQ.PRIME_ID = :B3 OR (QQQ.PRIME_ID IS NU` |
| 2 | gsyzv4fm3bgaw | 4068792643 | 66079 | 20028230727 | MODULE=PHPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   volcano1_base.pkg_aggregate_pack.packing_com(:p1, :p2, :p3); end; ` |
| 3 | 3urmsxt8b2ynf | 4028212274 | 8057370 | 20926818105 | MODULE=LPPUtk.exe | ACTION=None | PROG=6433906:37 | SCHEMA=PPA | `SELECT COUNT(1) FROM FORMBUNDLE FB WHERE FB.FH_ID = :B1 AND EXISTS (SELECT * FROM FORMPOS FP WHERE F` |
| 4 | 69bdttg3xqgur | 3989678788 | 130146 | 29981929449 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 5 | bbda8mncgc7ch | 3974658219 | 13531 | 10012941760 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | 9q1pujckt7byr | 3913752116 | 6711114 | 4662802711 | MODULE=LPPUtk.exe | ACTION=None | PROG=7949515:84 | SCHEMA=PPA | `SELECT COUNT(*) FROM V_DIC_TNVED T WHERE (T.KDPR = :B6 OR :B6 IS NULL) AND ((:B5 > T.BOR_MAX AND :B4` |
| 7 | gm7rhxav0k2kh | 3874892888 | 55739440 | 4254338214 | MODULE=None | PROG=73343:4 | SCHEMA=PPA | `SELECT MAX(C.ID) FROM COM C WHERE C.CON_STAGE=:B2 AND (ID IN (SELECT PARENT_ID FROM VOLCANO1_BASE.TR` |
| 8 | 954r7n97cx77h | 3860254896 | 33364684 | 20319043763 | MODULE=None | PROG=73561:5264 | SCHEMA=PPA | `SELECT MAX(ID) FROM (SELECT ID FROM (SELECT CON_STAGE, ID, DATE_BORN FROM COM C WHERE NOT EXISTS (SE` |
| 9 | fpuw6vc77xchv | 3850188097 | 8692 | 4149461393 | MODULE=LPPUtk.exe | ACTION=None | PROG=73614:3655 | SCHEMA=PPA | `SELECT /*+ RULE*/ NVL(MAX(LABEL_NUM), 0) + 1 FROM FORMPOS FP, FORMGROUP FG, SERT_S FS WHERE FP.FG_ID` |
| 10 | 8z35kjcm6uzx8 | 3839343585 | 135288 | 37440031339 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 11 | fjy4d0uu1nthm | 3792377430 | 135290 | 3005674794 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 12 | c9na3h8kkqjj1 | 3790110456 | 282 | 32519473144 | MODULE=PHPPStatist.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `with vbase as  (select /*+ ALL_ROWS */fil.ppr_rezerv,           fil.cnt_agr, fil.nom_time_agr,      ` |
| 13 | 37nbj8fcy8x53 | 3788504897 | 9607 | 22415550668 | MODULE=PHPPDisp.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `DECLARE BEGIN   ppa.pkg_dispreport_management_copy.load(trunc(:indate)); EXCEPTION   WHEN OTHERS THE` |
| 14 | 6vfzaz713bg4q | 3779528156 | 70839126 | 17296255626 | MODULE=LPPUtk.exe | ACTION=None | PROG=8510419:483 | SCHEMA=PPA | `SELECT MIN(COLUMN_VALUE) FROM TABLE(CAST(PKGNLMKGRAPH.GETID(:B1 ) AS T_ID_ARRAY))` |
| 15 | gtjmhhz1n8hq6 | 3774376038 | 27111362 | 5553869802 | MODULE=LPPFormer.exe | ACTION=None | PROG=7516195:10 | SCHEMA=PPA | `SELECT COUNT(1) FROM V_DIC_TNVED T WHERE T.KDPR = :B1 AND ROWNUM = 1` |

### Топ SQL по дисковым чтениям (DISK_READS)
| # | SQL_ID | DISK_READS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 828vhjjc42pb2 | 2267700775 | 27041 | 1615919227068 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 0kbn4faqa3c1g | 1826998674 | 135287 | 36290210590 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | fqw2yt57fh1rx | 1559233952 | 19785 | 8878449966 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 2cx3wa3vyjsqz | 1369067579 | 257786 | 67606349732 | MODULE=LPPFormer.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   PkgFormier_ud.AddComToForm(:p1, :p2, :p3, :p4); end; ` |
| 5 | 5yuyukc07zkwj | 1076920475 | 207199 | 48073252994 | MODULE=LPPFormer.exe | ACTION=None | PROG=73612:2761 | SCHEMA=PPA | `SELECT MAX(T.CON_PASSIVE) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS ID FROM TABLE(CAS` |
| 6 | 1gygbnwk3hru5 | 669765981 | 25671 | 587041190180 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 22xa3aws6wpgf | 503628182 | 2278298 | 458235795333 | MODULE=None | PROG=73612:2746 | SCHEMA=PPA | `SELECT MAX(NCON(T.CON_TYPE_PASSIVE, 462)) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS I` |
| 8 | 18a652xatt85j | 491372870 | 4734 | 4068760724 | MODULE=LPPAgregate.exe | ACTION=None | PROG=16225214:74 | SCHEMA=PPA | `UPDATE L2COMMON.WEIGHTS SET PRIME_ID = NULL, MSR_STATUS = NULL WHERE PRIME_ID = :B1 ` |
| 9 | 31f3bgx0twa7d | 455801888 | 1129 | 5691509181 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 10 | ds1vcqxdw4u6d | 381545778 | 6768 | 11032603110 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 11 | dpftnprx0whcu | 325305777 | 8511 | 13214832464 | MODULE=LPPFormer.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   PkgFormier_ud.UpdateTehParam(:p1, :p2, :p3); end; ` |
| 12 | 18p9x7wr540uk | 270882616 | 1748 | 242781070902 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 13 | 3chhgwuakxuzk | 267503450 | 13531 | 4048217933 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 14 | fsjm2x3as476u | 171683958 | 874 | 3618393139 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 15 | 6rpj3bk4cs91w | 115221531 | 1820 | 1497623576 | MODULE=PHPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `declare tmp number; begin   if :pack_type = 1 then     volcano1_base.PKG_AGGREGATE_PACK.PACKING_REMO` |

### Топ SQL по времени ожидания приложения (APPLICATION_WAIT_TIME)
| # | SQL_ID | APPLICATION_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 7b46rb1gupr3n | 2321356893294 | 61935071 | 36512598562 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PUHOV_ON | `begin  sys.dbms_alert.waitany(:name, :message, :status); end; ` |
| 2 | 1rzm7xsy69uzv | 209606469874 | 133 | 2057474680 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | 7p76ara3hshjn | 209454150645 | 29370 | 1579550341 | MODULE=ANGC1_PROC | ACTION=Event Processing | PROG=4247273:527 | SCHEMA=L2COMMON | `begin l2common.pangc1_proc.processOutputCoilState; end;` |
| 4 | 81z5p8g5fw0ut | 41142600594 | 115855 | 4091499 | MODULE=None | PROG=8752742:155 | SCHEMA=L2COMMON | `SELECT * FROM OUTCARTS WHERE OUTCART_CID = :B1 FOR UPDATE` |
| 5 | 1gf9w13xds7tt | 10830494083 | 22887 | 77033824755 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | bn3na5kr4qqm8 | 7705011883 | 2997 | 38346434194 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 06t9kvabnypxp | 5510807947 | 32582 | 31517027322 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | dsvp9hr900nhz | 5371006557 | 376492 | 1003231437 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   :doa__value := ppa.pkg_sap_operation.set_sap_operation(:p1, :p2); end; ` |
| 9 | b4vrkh03h1qkb | 5071311233 | 516207 | 12662379 | MODULE=PHPPAgregate.exe | ACTION=None | PROG=75039:520 | SCHEMA=VOLCANO1_BASE | `SELECT CHILD_ID, FACTOPER_ID FROM TRANS WHERE PARENT_ID = :B1 FOR UPDATE` |
| 10 | ap2ac5fhbudky | 3733174725 | 168409 | 782821550409 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по времени ожидания конкурентности (CONCURRENCY_WAIT_TIME)
| # | SQL_ID | CONCURRENCY_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 7b46rb1gupr3n | 151394274216 | 61935071 | 36512598562 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PUHOV_ON | `begin  sys.dbms_alert.waitany(:name, :message, :status); end; ` |
| 2 | ap2ac5fhbudky | 3244758670 | 168409 | 782821550409 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | bn3na5kr4qqm8 | 2702489032 | 2997 | 38346434194 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | gsyzv4fm3bgaw | 1942249411 | 66079 | 20028230727 | MODULE=PHPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   volcano1_base.pkg_aggregate_pack.packing_com(:p1, :p2, :p3); end; ` |
| 5 | 0g5b4j61q042t | 1731458795 | 1621200 | 75176412 | MODULE=LPPAgregate.exe | ACTION=None | PROG=3474:277 | SCHEMA=SYS | `UPDATE DBMS_ALERT_INFO SET CHANGED = 'N' WHERE SID = :B2 AND NAME = :B1 ` |
| 6 | 181yndn0jp46f | 1425473299 | 12339110 | 21315508696 | MODULE=oracle@nta1db1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=DB_MANAGER | `DECLARE   machinename VARCHAR2(64);   osuserid    VARCHAR2(30);   v_sid       NUMBER(10);   v_serial` |
| 7 | 2mzzwkhph0aw8 | 1124489281 | 51662 | 13815506655 | MODULE=PHPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   volcano1_base.pkg_aggregate_pack.packing_com(:p1, :p2, Null); end; ` |
| 8 | 0gqrdbu9d5j1h | 1017884962 | 9426450 | 3684770637 | MODULE=oracle@nta1db1 (TNS V1-V3) | ACTION=None | PROG=0:22 | SCHEMA=DB_MANAGER | `INSERT INTO DB_MANAGER.LOGONAUDITTABLE VALUES ('LOGON', :B6 , :B5 , :B4 , SYSDATE, USER, :B3 , :B2 ,` |
| 9 | 6q6u3ab0st2pj | 876140047 | 6093995 | 121218586 | MODULE=LPPAgregate.exe | ACTION=None | PROG=3474:273 | SCHEMA=SYS | `SELECT CHANGED, MESSAGE FROM DBMS_ALERT_INFO WHERE SID = :B2 AND NAME = :B1 ` |
| 10 | ad8hdkhx7v5ug | 749425975 | 135619126 | 6326625452 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `insert into "OBJEVENT"."AQ_OBJEVENT_TABLE"  (q_name, msgid, corrid, priority, state, delay, expirati` |

### Топ SQL по времени ожидания кластера (CLUSTER_WAIT_TIME)
| # | SQL_ID | CLUSTER_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | cv8n2prh2h000 | 0 | 3 | 113233 | MODULE=None | PROG=0:0 | SCHEMA=PPA | `select  /*+ no_index(I_COM_CON_STAGE_DATE_BORN)*/  PkgState.GET_STOREHOUSE(:v_Date,DATE_BORN,CON_STA` |
| 2 | a953ums1kh002 | 0 | 5 | 13139 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select ncon(t.CON_STAGE,1) STAGE,ncon(t.CON_STOREHOUSE,14) STOREHOUSE,ncon(t.RESIDUAL,10) NRESIDUAL,` |
| 3 | f93d9tvfv4002 | 0 | 7 | 8048 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select c.rowid,c.CON_ATTGRP,c.DELTAW,c.DIC_GOST,c.DIC_GRADE,c.LENGTH,c.CON_DEST_REPEAT,c.CON_SORT,c.` |
| 4 | bjc97tgyrs004 | 0 | 1 | 34238 | MODULE=LPPState.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select count(*), sum(WEIGHT), 0 from COM c, AKT a where c.AKT_ID_LAST = a.ID(+) and DATE_BORN > :v_D` |
| 5 | 10vpf0254c00f | 0 | 3 | 481 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select t.rowid,CON_SAP_KROM from com_ext t where (t.com_id = 64245989) and (t.rowid = :doa__rowid) ` |
| 6 | 01xkgkywrh00g | 0 | 1 | 78075 | MODULE=None | PROG=0:0 | SCHEMA=PPA | `select  /*+ no_index(I_COM_CON_STAGE_DATE_BORN)*/  PkgState.GET_STOREHOUSE(:v_Date,DATE_BORN,CON_STA` |
| 7 | gt1xr61wxn00h | 0 | 2 | 764 | MODULE=LPPAgregate.exe | ACTION=None | PROG=75039:635 | SCHEMA=VOLCANO1_BASE | `SELECT SUM(WEIGHT) FROM PPA.COM WHERE ID IN (64245893, 64245874, 64245878, 64245883, 64245888)` |
| 8 | 4ysgjsfwr000n | 0 | 77 | 8011 | MODULE=None | PROG=71494:260 | SCHEMA=L2COMMON | `update PPA.HLOT set DIC_TECH_KAR = '521566', SURFKKC = null, HOTFLAG = 0, CODE = 113, SLBTHICK = 250` |
| 9 | 94gav78zqw00r | 0 | 703 | 15493 | MODULE=None | PROG=11265497:372 | SCHEMA=VOLCANO1_BASE | `SELECT COUNT(1) FROM PPA.COM C WHERE C.ID = :B1 ` |
| 10 | 4930fmvh3s00r | 0 | 6 | 34992 | MODULE=plsql2svn.exe | ACTION=None | PROG=5028:2688 | SCHEMA=VOLCANO1_BASE | `SELECT /*+all_rows*/ VALUE(KU$), 0 FROM SYS.KU$_OBJGRANT_VIEW KU$ WHERE NOT BITAND(KU$.BASE_OBJ.FLAG` |

### Топ SQL по времени ожидания пользовательского IO (USER_IO_WAIT_TIME)
| # | SQL_ID | USER_IO_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 828vhjjc42pb2 | 767937711433 | 27041 | 1615919227068 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 31f3bgx0twa7d | 251323258932 | 1129 | 5691509181 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | 0kbn4faqa3c1g | 124913995120 | 135287 | 36290210590 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | grv7jrnzvt0xt | 71278081091 | 1304822470 | 28168318691 | MODULE=LPPFormer.exe | ACTION=None | PROG=3281742:128 | SCHEMA=PPA | `SELECT * FROM COM_EXT_ATTR T WHERE T.CON_ATTR_ID = :B3 AND T.COM_ID = :B2 AND NVL(T.BUNT_ID, -1) = N` |
| 5 | fsjm2x3as476u | 67317104003 | 874 | 3618393139 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | 18p9x7wr540uk | 65767499715 | 1748 | 242781070902 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 3chhgwuakxuzk | 44395245959 | 13531 | 4048217933 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | fqw2yt57fh1rx | 43735868712 | 19785 | 8878449966 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 9 | cus9620kqhk3h | 39359498199 | 874 | 1737316109 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 10 | 181yndn0jp46f | 37208556808 | 12339110 | 21315508696 | MODULE=oracle@nta1db1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=DB_MANAGER | `DECLARE   machinename VARCHAR2(64);   osuserid    VARCHAR2(30);   v_sid       NUMBER(10);   v_serial` |

### Топ SQL по физическим чтениям в байтах (PHYSICAL_READ_BYTES)
| # | SQL_ID | PHYSICAL_READ_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0kbn4faqa3c1g | 50151145226240 | 135287 | 36290210590 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 828vhjjc42pb2 | 18577004748800 | 27041 | 1615919227068 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | fqw2yt57fh1rx | 12773244534784 | 19785 | 8878449966 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 2cx3wa3vyjsqz | 11215401607168 | 257786 | 67606349732 | MODULE=LPPFormer.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   PkgFormier_ud.AddComToForm(:p1, :p2, :p3, :p4); end; ` |
| 5 | 5yuyukc07zkwj | 8822132531200 | 207199 | 48073252994 | MODULE=LPPFormer.exe | ACTION=None | PROG=73612:2761 | SCHEMA=PPA | `SELECT MAX(T.CON_PASSIVE) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS ID FROM TABLE(CAS` |
| 6 | 1gygbnwk3hru5 | 5486722916352 | 25671 | 587041190180 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 22xa3aws6wpgf | 4125722066944 | 2278298 | 458235795333 | MODULE=None | PROG=73612:2746 | SCHEMA=PPA | `SELECT MAX(NCON(T.CON_TYPE_PASSIVE, 462)) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS I` |
| 8 | 18a652xatt85j | 4025326551040 | 4734 | 4068760724 | MODULE=LPPAgregate.exe | ACTION=None | PROG=16225214:74 | SCHEMA=PPA | `UPDATE L2COMMON.WEIGHTS SET PRIME_ID = NULL, MSR_STATUS = NULL WHERE PRIME_ID = :B1 ` |
| 9 | 31f3bgx0twa7d | 3733929066496 | 1129 | 5691509181 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 10 | ds1vcqxdw4u6d | 3125623013376 | 6768 | 11032603110 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по физическим записям в байтах (PHYSICAL_WRITE_BYTES)
| # | SQL_ID | PHYSICAL_WRITE_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 1gygbnwk3hru5 | 58697039872 | 25671 | 587041190180 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | gaag4a9n8ksj4 | 55533305856 | 135169 | 144492005754 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | ap2ac5fhbudky | 42557603840 | 168409 | 782821550409 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 9pj66awucd7tn | 30043979776 | 259891 | 95343215871 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 5 | f82kmn1akbhr4 | 27487346688 | 1094837 | 778313744 | MODULE=None | PROG=21576065:1443 | SCHEMA=PPA | `INSERT INTO DOC_INTERCHANGE_LOG (DOC_ID, DATA, MESSAGE, RESPONSE, SEND_TIME) VALUES (:B5 , :B4 , :B3` |
| 6 | c9na3h8kkqjj1 | 21413068800 | 282 | 32519473144 | MODULE=PHPPStatist.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `with vbase as  (select /*+ ALL_ROWS */fil.ppr_rezerv,           fil.cnt_agr, fil.nom_time_agr,      ` |
| 7 | 1gf9w13xds7tt | 9855729664 | 22887 | 77033824755 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | 8m5bag9pv67r9 | 8399478784 | 6964 | 197129565 | MODULE=LPPFabricator.exe | ACTION=None | PROG=21400303:86 | SCHEMA=PLAN_MES | `SELECT DISTINCT COM_ID FROM (SELECT REPLACE(INVENTORYID, :B2 , NULL) AS COM_ID FROM (WITH PRZ AS (SE` |
| 9 | 0c77w3agh5vrs | 7648067584 | 656 | 194486803 | MODULE=LPPFabricator.exe | ACTION=None | PROG=75023:1227 | SCHEMA=VOLCANO1_BASE | `SELECT W.*, NVL((SELECT 1 FROM DUAL WHERE EXISTS (SELECT 1 FROM TRANS T WHERE EXISTS (SELECT 1 FROM ` |
| 10 | 49gyaa4r7uyt2 | 6834462720 | 1052171 | 324066757 | MODULE=None | PROG=15904293:791 | SCHEMA=PI | `WITH S AS ( SELECT PARENT_ID, PI.PKG_STOCK_UTL.GET_PARENT_ID(CHILD_ID) AS CHILD_ID, (SELECT COUNT(*)` |

### Топ SQL по байтам IO через interconnect (IO_INTERCONNECT_BYTES)
| # | SQL_ID | IO_INTERCONNECT_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0kbn4faqa3c1g | 50151145226240 | 135287 | 36290210590 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 828vhjjc42pb2 | 18577004748800 | 27041 | 1615919227068 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | fqw2yt57fh1rx | 12773244534784 | 19785 | 8878449966 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 2cx3wa3vyjsqz | 11215401607168 | 257786 | 67606349732 | MODULE=LPPFormer.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   PkgFormier_ud.AddComToForm(:p1, :p2, :p3, :p4); end; ` |
| 5 | 5yuyukc07zkwj | 8822132531200 | 207199 | 48073252994 | MODULE=LPPFormer.exe | ACTION=None | PROG=73612:2761 | SCHEMA=PPA | `SELECT MAX(T.CON_PASSIVE) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS ID FROM TABLE(CAS` |
| 6 | 1gygbnwk3hru5 | 5545419956224 | 25671 | 587041190180 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 22xa3aws6wpgf | 4125722066944 | 2278298 | 458235795333 | MODULE=None | PROG=73612:2746 | SCHEMA=PPA | `SELECT MAX(NCON(T.CON_TYPE_PASSIVE, 462)) FROM AKT_WIDTH T WHERE COM_ID IN (SELECT COLUMN_VALUE AS I` |
| 8 | 18a652xatt85j | 4025326551040 | 4734 | 4068760724 | MODULE=LPPAgregate.exe | ACTION=None | PROG=16225214:74 | SCHEMA=PPA | `UPDATE L2COMMON.WEIGHTS SET PRIME_ID = NULL, MSR_STATUS = NULL WHERE PRIME_ID = :B1 ` |
| 9 | 31f3bgx0twa7d | 3733929066496 | 1129 | 5691509181 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 10 | ds1vcqxdw4u6d | 3125623013376 | 6768 | 11032603110 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по байтам, подходящим под offload (IO_CELL_OFFLOAD_ELIGIBLE_BYTES)
| # | SQL_ID | IO_CELL_OFFLOAD_ELIGIBLE_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | cv8n2prh2h000 | 0 | 3 | 113233 | MODULE=None | PROG=0:0 | SCHEMA=PPA | `select  /*+ no_index(I_COM_CON_STAGE_DATE_BORN)*/  PkgState.GET_STOREHOUSE(:v_Date,DATE_BORN,CON_STA` |
| 2 | a953ums1kh002 | 0 | 5 | 13139 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select ncon(t.CON_STAGE,1) STAGE,ncon(t.CON_STOREHOUSE,14) STOREHOUSE,ncon(t.RESIDUAL,10) NRESIDUAL,` |
| 3 | f93d9tvfv4002 | 0 | 7 | 8048 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select c.rowid,c.CON_ATTGRP,c.DELTAW,c.DIC_GOST,c.DIC_GRADE,c.LENGTH,c.CON_DEST_REPEAT,c.CON_SORT,c.` |
| 4 | bjc97tgyrs004 | 0 | 1 | 34238 | MODULE=LPPState.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select count(*), sum(WEIGHT), 0 from COM c, AKT a where c.AKT_ID_LAST = a.ID(+) and DATE_BORN > :v_D` |
| 5 | 10vpf0254c00f | 0 | 3 | 481 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select t.rowid,CON_SAP_KROM from com_ext t where (t.com_id = 64245989) and (t.rowid = :doa__rowid) ` |
| 6 | 01xkgkywrh00g | 0 | 1 | 78075 | MODULE=None | PROG=0:0 | SCHEMA=PPA | `select  /*+ no_index(I_COM_CON_STAGE_DATE_BORN)*/  PkgState.GET_STOREHOUSE(:v_Date,DATE_BORN,CON_STA` |
| 7 | gt1xr61wxn00h | 0 | 2 | 764 | MODULE=LPPAgregate.exe | ACTION=None | PROG=75039:635 | SCHEMA=VOLCANO1_BASE | `SELECT SUM(WEIGHT) FROM PPA.COM WHERE ID IN (64245893, 64245874, 64245878, 64245883, 64245888)` |
| 8 | 4ysgjsfwr000n | 0 | 77 | 8011 | MODULE=None | PROG=71494:260 | SCHEMA=L2COMMON | `update PPA.HLOT set DIC_TECH_KAR = '521566', SURFKKC = null, HOTFLAG = 0, CODE = 113, SLBTHICK = 250` |
| 9 | 94gav78zqw00r | 0 | 703 | 15493 | MODULE=None | PROG=11265497:372 | SCHEMA=VOLCANO1_BASE | `SELECT COUNT(1) FROM PPA.COM C WHERE C.ID = :B1 ` |
| 10 | 4930fmvh3s00r | 0 | 6 | 34992 | MODULE=plsql2svn.exe | ACTION=None | PROG=5028:2688 | SCHEMA=VOLCANO1_BASE | `SELECT /*+all_rows*/ VALUE(KU$), 0 FROM SYS.KU$_OBJGRANT_VIEW KU$ WHERE NOT BITAND(KU$.BASE_OBJ.FLAG` |

### Топ SQL по числу физических чтений (PHYSICAL_READ_REQUESTS)
| # | SQL_ID | PHYSICAL_READ_REQUESTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 828vhjjc42pb2 | 1937726793 | 27041 | 1615919227068 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 31f3bgx0twa7d | 455801754 | 1129 | 5691509181 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | fsjm2x3as476u | 137224699 | 874 | 3618393139 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 18p9x7wr540uk | 127721479 | 1748 | 242781070902 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | 0kbn4faqa3c1g | 116549369 | 135287 | 36290210590 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | cus9620kqhk3h | 102136618 | 874 | 1737316109 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | grv7jrnzvt0xt | 76367611 | 1304822470 | 28168318691 | MODULE=LPPFormer.exe | ACTION=None | PROG=3281742:128 | SCHEMA=PPA | `SELECT * FROM COM_EXT_ATTR T WHERE T.CON_ATTR_ID = :B3 AND T.COM_ID = :B2 AND NVL(T.BUNT_ID, -1) = N` |
| 8 | b2wy4nq4cgaw9 | 60078779 | 45099 | 40117364135 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 9 | 547n7tqpt4fcy | 59161067 | 36644 | 34943498946 | MODULE=LPPUtk.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `begin   PkgForm_Utk_ud.AddNstsToFormPos(:p1, :p2); end; ` |
| 10 | dkg56jfbs49pt | 52115968 | 135366 | 1285673499 | MODULE=zabbix_proxy: odbc poller #3 [got 4 values in 0. | ACTION=None | PROG=0:0 | SCHEMA=ZABBIX | `SELECT  tablespace_name AS tablespace,  contents FROM DBA_TABLESPACES` |

### Топ SQL по числу физических записей (PHYSICAL_WRITE_REQUESTS)
| # | SQL_ID | PHYSICAL_WRITE_REQUESTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 1gygbnwk3hru5 | 6724856 | 25671 | 587041190180 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | gaag4a9n8ksj4 | 6534053 | 135169 | 144492005754 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | ap2ac5fhbudky | 4651691 | 168409 | 782821550409 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 9pj66awucd7tn | 3667478 | 259891 | 95343215871 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 5 | f82kmn1akbhr4 | 3132115 | 1094837 | 778313744 | MODULE=None | PROG=21576065:1443 | SCHEMA=PPA | `INSERT INTO DOC_INTERCHANGE_LOG (DOC_ID, DATA, MESSAGE, RESPONSE, SEND_TIME) VALUES (:B5 , :B4 , :B3` |
| 6 | 1gf9w13xds7tt | 1203092 | 22887 | 77033824755 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 49gyaa4r7uyt2 | 834285 | 1052171 | 324066757 | MODULE=None | PROG=15904293:791 | SCHEMA=PI | `WITH S AS ( SELECT PARENT_ID, PI.PKG_STOCK_UTL.GET_PARENT_ID(CHILD_ID) AS CHILD_ID, (SELECT COUNT(*)` |
| 8 | f8zbdazqjwckh | 566620 | 135287 | 8438300961 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 9 | 7hzmjmuzwf55t | 429206 | 67646 | 17012074062 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 10 | a9mmcdpbnw0av | 240094 | 240094 | 197434855 | MODULE=None | PROG=29515340:46 | SCHEMA=PPA | `WITH T_C AS (SELECT C.* FROM COM C WHERE ID = :B1 ) SELECT 1 FROM (SELECT T.TIME FROM TESTSTRENGTH T` |

### Топ SQL по обработанным строкам (ROWS_PROCESSED)
| # | SQL_ID | ROWS_PROCESSED | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 42wd7pfmkttf7 | 4039391195 | 1350486666 | 29617419957 | MODULE=None | PROG=21576064:729 | SCHEMA=PPA | `SELECT SUBSTR(STR, INSTR(STR, '/', DECODE(LEVEL, 1, 0, 1), GREATEST(LEVEL - 1, 1)) + 1, DECODE(CONNE` |
| 2 | 0y0bshnsvqkb8 | 3065988658 | 3835993641 | 26336368123 | MODULE=LPPUtk.exe | ACTION=None | PROG=73529:19 | SCHEMA=PPA | `SELECT PARENT_ID FROM V_TRANS T WHERE CHILD_ID = :B1 ` |
| 3 | 4s71mb2gjbbkt | 1972348681 | 83867115 | 2325721460 | MODULE=None | PROG=21576064:1014 | SCHEMA=PPA | `SELECT * FROM DOC_INTERCHANGE_ATTR WHERE TAG_ID = :B1 ORDER BY CODE` |
| 4 | 3ugjrunwsncwz | 1433068680 | 1433069139 | 5194723159 | MODULE=None | PROG=71681:146 | SCHEMA=OBJEVENT | `select object_id, last_event from EVENT_params where last_event = (select min (last_event) from EVEN` |
| 5 | 71jmmngj8dfmb | 1413347330 | 19337048 | 4270520768 | MODULE=None | PROG=1342421:271 | SCHEMA=TM | `SELECT T.* FROM TM_T_TASKS T, TM_THR_GROUPS TG WHERE T.GROUP_ID = TG.GROUP_ID AND (T.COUNT_EXEC = 0 ` |
| 6 | cmprrupqvkx7t | 1167648034 | 1167648694 | 2729725658 | MODULE=PHPPAgregate.exe | ACTION=None | PROG=3916106:7 | SCHEMA=PPA | `SELECT COUNT(1) FROM PACK_COM_DISCARD T WHERE T.COM_ID = :B1 AND T.ENT_ID_CANCEL IS NULL AND ROWNUM ` |
| 7 | 05z0ax4bv0f71 | 832806563 | 682554264 | 4734391945 | MODULE=LPPUtk.exe | ACTION=None | PROG=73529:10 | SCHEMA=PPA | `SELECT CHILD_ID FROM V_TRANS T WHERE PARENT_ID = :B1 ` |
| 8 | 54bcnvmggt692 | 703498882 | 703499217 | 1722204176 | MODULE=None | PROG=466592:630 | SCHEMA=VOLCANO1_BASE | `SELECT A.CON_COVERING FROM PPA.APROLL A WHERE A.ID = :B1 ` |
| 9 | 67q5v28gkr3n1 | 700057539 | 19782237 | 4369838241 | MODULE=None | PROG=71486:66 | SCHEMA=L2COMMON | `SELECT ATTRNUMBER NO, ATTRID ID FROM ARPL.USABILITY U WHERE DOCTYPEID = :B1 ORDER BY ATTRNUMBER` |
| 10 | 4z6jfdhm5uxr7 | 547707322 | 554573559 | 2695187768 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `select longname from javasnm$ where short = :1` |

### Топ SQL по сортировкам (SORTS)
| # | SQL_ID | SORTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 97tjyz36u5kh1 | 91966175 | 91964762 | 610012373 | MODULE=LPPAgregate.exe | ACTION=None | PROG=7428682:7 | SCHEMA=PPA | `SELECT CON_REPROCESSING_DIRECTION FROM (SELECT A.CON_REPROCESSING_DIRECTION FROM REPROCESSING_ATTR A` |
| 2 | 64mczfb48vd69 | 91671025 | 91670860 | 10804765884 | MODULE=None | PROG=73561:6423 | SCHEMA=PPA | `SELECT E.TEAM FROM ( SELECT E.TEAM, ROW_NUMBER() OVER ( ORDER BY E.TREATDATE DESC, M.ID DESC ) AS RN` |
| 3 | 4s71mb2gjbbkt | 83867165 | 83867115 | 2325721460 | MODULE=None | PROG=21576064:1014 | SCHEMA=PPA | `SELECT * FROM DOC_INTERCHANGE_ATTR WHERE TAG_ID = :B1 ORDER BY CODE` |
| 4 | ffwdu2uhhugfr | 80114571 | 40057281 | 3331809387 | MODULE=LPPFormer.exe | ACTION=None | PROG=73604:3252 | SCHEMA=PPA | `SELECT ID FROM (SELECT R.ID FROM COM R, COM C, V_TRANS T WHERE R.ID = T.PARENT_ID AND T.CHILD_ID = C` |
| 5 | c3mhyns2h7k0g | 75787743 | 251615292 | 5772606931 | MODULE=LPPFormer.exe | ACTION=None | PROG=1656066:10 | SCHEMA=PPA | `SELECT * FROM COM_EXT_SPEC T WHERE T.COM_ID = :B1 ORDER BY T.SPEC_ID` |
| 6 | a9yx1hgx2rt63 | 58639394 | 29319855 | 9516969262 | MODULE=None | PROG=73561:4993 | SCHEMA=PPA | `SELECT COUNT(*), NCON(PKGLPPPASSPORT.CON_STAGE_RECODE(M.CON_STAGE, M.CON_UNIT, M.TNUM, M.ID), 278) A` |
| 7 | 36yab2aarhrsr | 54046290 | 54045561 | 594028392 | MODULE=LPPAgregate.exe | ACTION=None | PROG=3001796:2520 | SCHEMA=PPA | `SELECT /*+ RULE */ COUNT(*) FROM (SELECT T.PARENT_ID FROM VOLCANO1_BASE.COMTRANS TR, COM C, COM C_CH` |
| 8 | dnvrqa2s1dmm8 | 44280324 | 22140162 | 1005117527 | MODULE=None | PROG=28765992:708 | SCHEMA=PPA | `SELECT C.STAGE_ID ,C.CON_STAGE ,C.ID FROM PPA.COM C WHERE C.ID IN (SELECT PARENT_ID FROM VOLCANO1_BA` |
| 9 | 2cv4ar5457gx4 | 40015942 | 40015694 | 471032402 | MODULE=None | PROG=6967778:8 | SCHEMA=PPA | `SELECT CON_REPROCESSING_DIRECTION FROM REPROCESSING_ATTR A WHERE A.COM_ID = :B1 ORDER BY A.DATE_REPR` |
| 10 | 3uf5w9bq33zc8 | 36372485 | 36371983 | 1844583620 | MODULE=None | PROG=73561:3440 | SCHEMA=PPA | `SELECT MAX(PASSFLAG) FROM (SELECT C2.ID, DECODE(NVL(AGC.PASSFLAG, 0), 0, NULL, 1) PASSFLAG FROM AGCR` |

### Топ SQL по числу парсингов (PARSE_CALLS)
| # | SQL_ID | PARSE_CALLS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0y0bshnsvqkb8 | 3125620840 | 3835993641 | 26336368123 | MODULE=LPPUtk.exe | ACTION=None | PROG=73529:19 | SCHEMA=PPA | `SELECT PARENT_ID FROM V_TRANS T WHERE CHILD_ID = :B1 ` |
| 2 | 05z0ax4bv0f71 | 244996215 | 682554264 | 4734391945 | MODULE=LPPUtk.exe | ACTION=None | PROG=73529:10 | SCHEMA=PPA | `SELECT CHILD_ID FROM V_TRANS T WHERE PARENT_ID = :B1 ` |
| 3 | 5jb4dk5s8dkz7 | 188012976 | 213765096 | 2238491760 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=7551757:20 | SCHEMA=PPA | `SELECT T.PARENT_ID FROM VOLCANO1_BASE.TRANS T WHERE T.CHILD_ID = :B1 ` |
| 4 | 2jfqzrxhrm93b | 178563810 | 178563794 | 1923552575 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select /*+ rule */ c.name, u.name from con$ c, cdef$ cd, user$ u  where c.con# = cd.con# and cd.enab` |
| 5 | 3dg0j9n55b71w | 155716714 | 259457483 | 2838854024 | MODULE=None | PROG=73582:93 | SCHEMA=PPA | `SELECT * FROM PPA.COM WHERE ID = :B1 ` |
| 6 | 7ztvwb22m53mz | 94762846 | 106193966 | 802313486 | MODULE=None | PROG=73561:3307 | SCHEMA=PPA | `SELECT STAGE_ID FROM COM WHERE ID = :B1 ` |
| 7 | dtmncf0h7dgba | 87613396 | 324461171 | 2310796199 | MODULE=LPPFormer.exe | ACTION=None | PROG=73516:97 | SCHEMA=PPA | `SELECT NAME FROM CONS WHERE DIC = :B2 AND ID = :B1 AND DATE_END IS NULL AND ROWNUM = 1 ` |
| 8 | cm5vu20fhtnq1 | 83326559 | 83272102 | 1537132240 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select /*+ connect_by_filtering */ privilege#,level from sysauth$ connect by grantee#=prior privileg` |
| 9 | 1v717nvrhgbn9 | 68069146 | 133190674 | 871130472 | MODULE=oracle@nta1db1 (TNS V1-V3) | ACTION=None | PROG=649:180 | SCHEMA=TIGER | `SELECT USER FROM SYS.DUAL` |
| 10 | c3mhyns2h7k0g | 66863508 | 251615292 | 5772606931 | MODULE=LPPFormer.exe | ACTION=None | PROG=1656066:10 | SCHEMA=PPA | `SELECT * FROM COM_EXT_SPEC T WHERE T.COM_ID = :B1 ORDER BY T.SPEC_ID` |

### Топ SQL по занимаемой памяти в пуле (SHARABLE_MEM)
| # | SQL_ID | SHARABLE_MEM | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | badfbqz95fvnb | 1092634234 | 1368 | 598796558 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select  t.MACHINE_NAME,  t.DIC_STAGE,  t.ASSEMBLY,  t.MACHINE_ID,  t.MACHINE_ID AGRID_TO,  t.TIME_BE` |
| 2 | gp8wx5pyj5c91 | 1046556535 | 1166 | 473695195 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select sum(DT_DURATION) as DT_DURATION_24741 from (select  t.MACHINE_NAME,  t.DIC_STAGE,  t.ASSEMBLY` |
| 3 | ggu8xspju1vvh | 82570929 | 1120 | 14770027 | MODULE=SQL*Plus | ACTION=None | PROG=5028:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+all_rows*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('TABLE_T', '7')), KU$.OBJ_NUM FR` |
| 4 | 9jk5afyzdrcht | 47753541 | 495 | 6757828 | MODULE=SQL*Plus | ACTION=None | PROG=5028:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+rule*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('M_VIEW_T', '7')), KU$.MVIEW_TAB.OBJ` |
| 5 | awznpcqt0db2d | 44449194 | 21 | 7827674 | MODULE=SQL*Plus | ACTION=None | PROG=5028:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+all_rows*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('TABLE_T', '7')), KU$.OBJ_NUM FR` |
| 6 | 48s82gxsc2bfq | 41517969 | 87 | 5931438 | MODULE=SQL*Plus | ACTION=None | PROG=5028:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+all_rows*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('TABLE_T', '7')), KU$.OBJ_NUM FR` |
| 7 | 4p6k55v0d1qf9 | 35554390 | 544536 | 36227854054 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPCCM4 | `SELECT "ID3","ATTRID","MIN","MAX","AVG","DEV","VAL_STR","VAL_CODE","VAL_DATE" FROM "VOLCANO1_BASE"."` |
| 8 | 6g3b29319p4ut | 32030603 | 14152 | 395079399 | MODULE=None | PROG=0:0 | SCHEMA=PPA | `select /* FUNCTION_FIELD */ t.*,        --a.min as kl_izm_s,                          --RR-11466 E.P` |
| 9 | 1m1v635c2zb8j | 25213019 | 39394 | 1330511090 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPCCM4 | `SELECT /*+ ALL_ROWS */ "ID3" FROM "VOLCANO1_BASE"."SUP_PARAMETERS_FULL" "T" WHERE "ID3"=:1` |
| 10 | 12g26t5rnjk0n | 21638754 | 9700 | 222757323 | MODULE=LPPAgregate.exe | ACTION=None | PROG=75089:4630 | SCHEMA=VOLCANO1_BASE | `WITH BASE_T AS ( SELECT T.* FROM ( SELECT RAR.ID ,RAR.CON_TREAT ,RAR.PRIZN_GILZA ,RAR.OILING ,RAR.PA` |

### Топ SQL по числу версий (VERSION_COUNT)
| # | SQL_ID | VERSION_COUNT | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 2pnhv18x45psb | 1016 | 0 | 51745 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 2 | 6qtprayc6bk45 | 840 | 0 | 33841 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 3 | 8v5yrk5c4zsfd | 840 | 0 | 35814 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 4 | f42xk7bwvnuh4 | 747 | 0 | 21147 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 5 | 3nkd3g3ju5ph1 | 399 | 44882 | 2084398 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select obj#,type#,ctime,mtime,stime, status, dataobj#, flags, oid$, spare1, spare2 from obj$ where o` |
| 6 | 7ng34ruy5awxq | 386 | 30092 | 4564535 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select i.obj#,i.ts#,i.file#,i.block#,i.intcols,i.type#,i.flags,i.property,i.pctfree$,i.initrans,i.ma` |
| 7 | cx0p7twfbd2sk | 365 | 0 | 16020 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 8 | 1mxazkk4gzny1 | 322 | 747455 | 51179156 | MODULE=oracle@nta1db1 (TNS V1-V3) | ACTION=None | PROG=0:82 | SCHEMA=TIGER | `UPDATE XPARSE SET WAS = SYSDATE, H = :B14 , N01 = :B13 ,N02 = :B12 , N03 = :B11 , N04 = :B10 ,N05 = ` |
| 9 | 8tvuyc523bqq3 | 291 | 679602 | 20145357 | MODULE=oracle@nta1db1 (TNS V1-V3) | ACTION=None | PROG=0:73 | SCHEMA=TIGER | `SELECT USID FROM XPARSE WHERE USID = UPPER(:B1 )` |
| 10 | 6gkwcvqz5ypr0 | 272 | 985459 | 41953857 | MODULE=oracle@nta1db1 (TNS V1-V3) | ACTION=None | PROG=0:36 | SCHEMA=TIGER | `SELECT WAS, N01,N02,N03,N04,N05,N06,N07,N08,N09,N10,N11,N12 FROM XPARSE WHERE USID = :B1 ` |

### Нагрузка по источнику (MODULE)
| MODULE | SQL | Executions | CPU (ms) | Elapsed (ms) | Buffer | Disk | RowsProc |
|--------|----:|-----------:|---------:|-------------:|-------:|-----:|--------:|
| None | 10887 | 13884817831 | 5299833772 | 10554725555 | 206706093093 | 9653094551 | 18507900341 |
| oracle@nl-sp-mes-db3 (TNS V1-V3) | 219 | 787856497 | 602294525 | 633403966 | 12862311219 | 20303005 | 725779706 |
| LPPFormer.exe | 732 | 3836981501 | 304023251 | 444232316 | 35670338011 | 2900379347 | 3092695174 |
| EnterAkt.exe | 2383 | 833628872 | 189180575 | 226555885 | 17683541598 | 174860584 | 687028327 |
| LPPUtk.exe | 2483 | 4713176227 | 171102212 | 667758943 | 30616340998 | 154127323 | 4086122992 |
| LPPAgregate.exe | 23322 | 1820149850 | 143410295 | 2735475569 | 41764328981 | 529073704 | 1784148828 |
| oracle@nta1db1 (TNS V1-V3) | 13 | 363876385 | 58513431 | 134299170 | 829047512 | 26693880 | 363905463 |
| PHPPReportForm.exe | 129 | 8912281 | 58171060 | 66842590 | 3800274927 | 6029026 | 61873690 |
| PHPPAgregate.exe | 1739 | 1226925780 | 50882334 | 103013074 | 12458946104 | 164198865 | 1232195271 |
| oracle@nl-sp-qidb-db1 (TNS V1-V3) | 30 | 7702901 | 46258774 | 49591570 | 10075908974 | 4369175 | 6197758 |
| PHPPStatist.exe | 27 | 22771345 | 40969028 | 53530409 | 7076891901 | 3312765 | 23762264 |
| PHPPDisp.exe | 123 | 103394830 | 25416038 | 31037864 | 4231195449 | 50586130 | 103809815 |
| JDBC Thin Client | 687 | 592292318 | 21497840 | 26235949 | 3825380565 | 1848160 | 912313108 |
| LPPOrders.exe | 809 | 619448096 | 18383898 | 24341676 | 7438040925 | 6418859 | 541608807 |
| LPPFabricator.exe | 1063 | 147287883 | 15198675 | 20735997 | 6206772434 | 161065962 | 104849222 |
| LPPPassport.exe | 624 | 579874145 | 11355555 | 15793168 | 6201263377 | 7769765 | 474624266 |
| LPPSummary.exe | 682 | 335619 | 10842884 | 17345538 | 4448573862 | 10131270 | 386828 |
| LPPState.exe | 367 | 207132166 | 10793164 | 20837475 | 5101760086 | 6032176 | 59527752 |
| PL/SQL Developer | 158 | 19556855 | 8080923 | 9279241 | 1415560021 | 41625722 | 142944660 |
| DS1_PROC | 106 | 480786 | 7806789 | 10530066 | 3612853861 | 2666170 | 473659 |

### Нагрузка по схеме парсинга (PARSING_SCHEMA_NAME)
| SCHEMA | SQL | Executions | CPU (ms) | Elapsed (ms) | Buffer | Disk |
|--------|----:|-----------:|---------:|-------------:|-------:|-----:|
| TM | 250 | 60671797 | 4192717960 | 8003908253 | 81787963864 | 8629220427 |
| PPA | 36087 | 20807526197 | 2365357952 | 3719147897 | 269880029250 | 4983765620 |
| L2COMMON | 2593 | 142879345 | 122845190 | 1068768852 | 25031540914 | 108278015 |
| SYS | 3160 | 1516875310 | 102015348 | 156636010 | 11627494423 | 42473015 |
| DB_MANAGER | 16 | 50811830 | 55148745 | 129789557 | 394424390 | 27225136 |
| VOLCANO1_BASE | 17842 | 4728445193 | 54724272 | 96907395 | 14859756460 | 69780505 |
| VOLCANO1_SOUP | 249 | 27591476 | 53170943 | 56615377 | 3939745530 | 546971 |
| SUPCCM4 | 73 | 57248332 | 42142384 | 45636664 | 1665180950 | 1198109 |
| PUHOV_ON | 2 | 61955641 | 36513509 | 2578361014 | 2464567281 | 24044 |
| ARPL | 135 | 20615668 | 30310881 | 103858091 | 5300619940 | 4656747 |
| LINK_QIDB | 19 | 3464895 | 28982969 | 31759553 | 3300374480 | 3633485 |
| PLAN_BASE | 239 | 17670868 | 17992012 | 67429197 | 6207785784 | 60055853 |
| PI | 241 | 599559847 | 14263289 | 28405378 | 2935696217 | 33277333 |
| OBJEVENT | 262 | 1833210674 | 8698987 | 12374099 | 2097393109 | 4865828 |
| SM_ANGC1 | 4 | 1120226 | 6194800 | 24480266 | 1008466181 | 1121 |

### Нагрузка по типу команды (COMMAND_TYPE)
| COMMAND_TYPE | SQL | Executions | CPU (ms) |
|--------------|----:|-----------:|---------:|
| 47 | 1071 | 482483719 | 4782212808 |
| 3 | 56134 | 28996987806 | 2288639420 |
| 2 | 1039 | 333599156 | 33148584 |
| 6 | 4664 | 246937273 | 27417495 |
| 7 | 606 | 129915850 | 16112759 |
| 170 | 8 | 37252 | 7093167 |
| 189 | 59 | 7227065 | 3379557 |
| 90 | 2 | 5153801 | 113540 |
| 48 | 1 | 3572835 | 11149 |
| 26 | 154 | 176592 | 3467 |

### Агрегаты по пулу
- Всего SQL: **63832**
- Суммарно исполнений: **30206091443**
- CPU: **7158132239 ms**
- Время выполнения: **16191699202 ms**
- Буферные чтения: **440767150347**
- Дисковые чтения: **14285814703**
- Сортировки: **1281499218**
- Парсинги (PARSE_CALLS): **7745551268**
- Обработанные строки (ROWS_PROCESSED): **33672773977**
- Занимаемая память пула (SHARABLE_MEM): **11.0 GB**

### SQL с высоким VERSION_COUNT (>20) — кандидаты на проблему версионирования
| SQL_ID | VersionCount | Источник | SQL |
|--------|-------------:|----------|-----|
| cat9kz4nkn0a3 | 23 | MODULE=None | PROG=19673966:5 | SCHEMA=SYS | `INSERT INTO ADM.AUD_HIST SELECT * FROM SYS.AUD$ WHERE NTIMESTAMP# < :B1 ` |
| bjt6vk5shc0bm | 27 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select rowid, msgid, subscriber#, name, address#, dequeue_time, transaction_id, dequeue_user, flags ` |
| db245ndxpn0rw | 24 | MODULE=None | PROG=21576065:1489 | SCHEMA=PPA | `INSERT INTO DOC_INTERCHANGE_LOG_KEY (LOG_ID, KEY, TS, OP, RESPONSE, DATA) VALUES (:B6 , :B5 , :B4 , ` |
| dmyg24faac14x | 55 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=71486:684 | SCHEMA=L2COMMON | `DELETE L2COMMON.TRENDS_MES WHERE PRIME_ID = :B2 AND FORMAT_ID = :B1 ` |
| fw0c8uc34c16p | 23 | MODULE=None | PROG=0:0 | SCHEMA=PLAN_BASE | `INSERT INTO "PLAN_BASE"."MV_PO_TASK_OUTPUT"  ("TASKID","OUTPUTMATERIALID","SEQNR","QUANTITY","PRODUC` |
| 81aj4367ac18z | 63 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `UPDATE sys.snap$ SET status = :1, rscn = :2, refhnt = :3,         flag = :4, flag3 = :5  WHERE vname` |
| 7xb4axhqds1fb | 28 | MODULE=zabbix_proxy: odbc poller #1 [got 1 values in 0. | ACTION=None | PROG=0:0 | SCHEMA=ZABBIX | `SELECT 'SYS::' || METRIC_NAME AS METRIC, ROUND(VALUE,3) as VALUE   FROM GV$SYSMETRIC WHERE GROUP_ID ` |
| 28vmfpk12n23w | 53 | MODULE=None | PROG=4111:2232 | SCHEMA=SYS | `SELECT SUBSTR(MLINK,2), TNAME, USLOG, SUBSTR(AUTO_FAST,1,1), BITAND(FLAG, :B4 ), BITAND(FLAG, :B3 ) ` |
| 3xvszy309s2d3 | 24 | MODULE=None | PROG=0:0 | SCHEMA=PLAN_BASE | ` delete from "PLAN_BASE"."MV_TO_OPERATION"` |
| 37zpqvfz442xw | 21 | MODULE=None | PROG=0:0 | SCHEMA=PLAN_BASE | ` delete from "PLAN_BASE"."MV_ATTR_DIC"` |

### SQL с инвалидациями (INVALIDATIONS>0)
| SQL_ID | Invalidations | Источник | SQL |
|--------|--------------:|----------|-----|
| 94gav78zqw00r | 11 | MODULE=None | PROG=11265497:372 | SCHEMA=VOLCANO1_BASE | `SELECT COUNT(1) FROM PPA.COM C WHERE C.ID = :B1 ` |
| bvgbb7w4a401z | 6 | MODULE=EnterAkt.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select t.rowid, t.* from APROLL t where (ID =:ID) and (t.rowid = :doa__rowid) ` |
| 04yj54wtxc032 | 1 | MODULE=LPPFabricator.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `SELECT volcano1_base.pkgfabricator.prodprog_approval_com(c.id) com_pro_approval,ppa.get_act_attgrp(c` |
| 108wn79jhn046 | 1 | MODULE=None | PROG=0:0 | SCHEMA=PPA | `insert into FORMGROUP  (CON_PRODKIND, CON_PACK, CON_SHEMA, DIC_KIND, FH_ID, POS_ID) values  (:CON_PR` |
| 4xmcd6ykw804c | 67 | MODULE=LPPAgregate.exe | ACTION=None | PROG=73559:322 | SCHEMA=PPA | `UPDATE COM SET COM_ID=:B2 WHERE ID=:B1 ` |
| 74c3873dxn04t | 8 | MODULE=LPPFabricator.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `update PPA.COMREAL_LIVE set JOB_ID = :JOB_ID, CSJOB_ID = null, CON_BINDSTATE = :CON_BINDSTATE where ` |
| c13jhwxsdw05w | 2 | MODULE=None | PROG=0:0 | SCHEMA=PPA | `select ndic(t.DIC_GOST1) GOST1,ndic(t.DIC_GOST2) GOST2,ndic(t.DIC_GOSTM) GOSTM,ndic(t.DIC_SUPCOND) S` |
| 74cwgs8d0w06b | 446 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPCCM4 | `SELECT /*+ FULL(P) +*/ * FROM "VOLCANO1_BASE"."SUP_PARAMETERS_FULL" P` |
| 6qj4b07hhh074 | 1 | MODULE=None | PROG=0:0 | SCHEMA=PPA | `select ncon(t.CON_STOREHOUSE,14) STOREHOUSE,ncon(t.RESIDUAL,10) NRESIDUAL,ndic(t.DIC_GRADE) GRADE,nd` |
| g0b94r8mth07q | 7 | MODULE=LPPAgregate.exe | ACTION=None | PROG=0:0 | SCHEMA=PPA | `select * from PPA.COMREAL where rowid = :doa__rowid ` |

---

## ccm4 / table_sizes

_Файл_: `./spider-output/ccm4_table_sizes.json`, строк: 266

_Колонки_: SEGMENT_NAME (VARCHAR2), SEGMENT_TYPE (VARCHAR2), BYTES (NUMBER)

**Сегменты**: 266 шт, суммарно **99.2 GB**

### Топ сегментов по размеру
| # | SEGMENT_NAME | SEGMENT_TYPE | BYTES |
|---|--------------|--------------|------:|
| 1 | INCARTS_LOG | TABLE PARTITION | 19842269184 (18.5 GB) |
| 2 | TIME_TRENDS | TABLE | 14826864640 (13.8 GB) |
| 3 | OUTCARTS_LOG | TABLE PARTITION | 14758903808 (13.7 GB) |
| 4 | MEASUREMENTS_PK | INDEX PARTITION | 7700938752 (7.2 GB) |
| 5 | INCARTS_LOG_PK | INDEX | 6457131008 (6.0 GB) |
| 6 | SYS_LOB0008026661C00002$$ | LOBSEGMENT | 5955911680 (5.5 GB) |
| 7 | OUTCARTS_LOG_PK | INDEX | 4953473024 (4.6 GB) |
| 8 | INCARTS_LOG_INCART_CID_FK | INDEX | 4734320640 (4.4 GB) |
| 9 | OUTCARTS_LOG_OUTCART_CID_FK | INDEX | 4257218560 (4.0 GB) |
| 10 | LEN_TRENDS | TABLE PARTITION | 3239641088 (3.0 GB) |
| 11 | WEIGHTS_PK | INDEX | 2124414976 (2.0 GB) |
| 12 | PK_PARAMETERS | INDEX PARTITION | 2090926080 (1.9 GB) |
| 13 | BUNDLES_LOG | TABLE | 1547698176 (1.4 GB) |
| 14 | WEIGHTS | TABLE PARTITION | 1455423488 (1.4 GB) |
| 15 | PK_TIME_TRENDS | INDEX | 1384120320 (1.3 GB) |
| 16 | SYS_IOT_OVER_70668 | TABLE PARTITION | 800391168 (763.3 MB) |
| 17 | MARKING_BASKET | TABLE PARTITION | 799735808 (762.7 MB) |
| 18 | SYS_LOB0000071120C00008$$ | LOB PARTITION | 798556160 (761.6 MB) |
| 19 | BUNDLES_LOG_BUNDLE_CID_FK | INDEX | 766509056 (731.0 MB) |
| 20 | OUTCARTS | TABLE PARTITION | 522190848 (498.0 MB) |

### Размер по типу сегмента
| SEGMENT_TYPE | Суммарно |
|--------------|---------:|
| TABLE PARTITION | 39.4 GB |
| INDEX | 27.9 GB |
| TABLE | 16.0 GB |
| INDEX PARTITION | 9.6 GB |
| LOBSEGMENT | 5.6 GB |
| LOB PARTITION | 761.7 MB |
| LOBINDEX | 9.9 MB |

---

## ccm4 / tablespaces

_Файл_: `./spider-output/ccm4_tablespaces.json`, строк: 32

_Колонки_: TABLESPACE_NAME (VARCHAR2), BYTES (NUMBER)

**Табличные пространства**: 32 шт

| NAME | TYPE | SIZE | FREE | USED % |
|------|------|-----:|-----:|-------:|
| PPATABL |  | 275.5 GB | 275.5 GB | 0.0% |
| PPAINDX |  | 143.6 GB | 143.6 GB | 0.0% |
| TECHTABL |  | 68.7 GB | 68.7 GB | 0.0% |
| L2TABL |  | 67.2 GB | 67.2 GB | 0.0% |
| PIINDX |  | 38.4 GB | 38.4 GB | 0.0% |
| PITABL |  | 35.0 GB | 35.0 GB | 0.0% |
| UNDOTBS1 |  | 32.0 GB | 32.0 GB | 0.0% |
| LOGTABL |  | 28.9 GB | 28.9 GB | 0.0% |
| QLOGTABL |  | 28.6 GB | 28.6 GB | 0.0% |
| VLCTABL |  | 19.2 GB | 19.2 GB | 0.0% |
| USERS |  | 17.5 GB | 17.5 GB | 0.0% |
| PLANINDX |  | 14.0 GB | 14.0 GB | 0.0% |
| VLCINDX |  | 11.5 GB | 11.5 GB | 0.0% |
| ETLTABL |  | 10.3 GB | 10.3 GB | 0.0% |
| PLANTABLE |  | 7.9 GB | 7.9 GB | 0.0% |
| TPARTABL |  | 7.1 GB | 7.1 GB | 0.0% |
| SYSAUX |  | 5.6 GB | 5.6 GB | 0.0% |
| ARPLTABL |  | 2.2 GB | 2.2 GB | 0.0% |
| SYSTEM |  | 2.2 GB | 2.2 GB | 0.0% |
| INDX |  | 1.8 GB | 1.8 GB | 0.0% |
| ARPLINDX |  | 1.0 GB | 1.0 GB | 0.0% |
| TOOLS |  | 435.0 MB | 435.0 MB | 0.0% |
| L2QUE |  | 352.0 MB | 352.0 MB | 0.0% |
| DB2KAFKATABL |  | 220.0 MB | 220.0 MB | 0.0% |
| L2KAFKATABL |  | 200.0 MB | 200.0 MB | 0.0% |
| MESQUE |  | 126.0 MB | 126.0 MB | 0.0% |
| XDB |  | 58.0 MB | 58.0 MB | 0.0% |
| CWMLITE |  | 16.0 MB | 16.0 MB | 0.0% |
| ODM |  | 10.0 MB | 10.0 MB | 0.0% |
| DRSYS |  | 8.0 MB | 8.0 MB | 0.0% |
| PLANTABL |  | 8.0 MB | 8.0 MB | 0.0% |
| TEMP |  | 0.0 B | 0.0 B | 0.0% |

---

## ccm5 / shared_pool

_Файл_: `./spider-output/ccm5_shared_pool.json`, строк: 16200

_Колонки_: SQL_TEXT (VARCHAR2), SQL_ID (VARCHAR2), SHARABLE_MEM (NUMBER), PERSISTENT_MEM (NUMBER), RUNTIME_MEM (NUMBER), SORTS (NUMBER), VERSION_COUNT (NUMBER), LOADED_VERSIONS (NUMBER), OPEN_VERSIONS (NUMBER), USERS_OPENING (NUMBER), FETCHES (NUMBER), EXECUTIONS (NUMBER), PX_SERVERS_EXECUTIONS (NUMBER), END_OF_FETCH_COUNT (NUMBER), USERS_EXECUTING (NUMBER), LOADS (NUMBER), FIRST_LOAD_TIME (VARCHAR2), INVALIDATIONS (NUMBER), PARSE_CALLS (NUMBER), DISK_READS (NUMBER), DIRECT_WRITES (NUMBER), BUFFER_GETS (NUMBER), APPLICATION_WAIT_TIME (NUMBER), CONCURRENCY_WAIT_TIME (NUMBER), CLUSTER_WAIT_TIME (NUMBER), USER_IO_WAIT_TIME (NUMBER), PLSQL_EXEC_TIME (NUMBER), JAVA_EXEC_TIME (NUMBER), ROWS_PROCESSED (NUMBER), COMMAND_TYPE (NUMBER), OPTIMIZER_MODE (VARCHAR2), OPTIMIZER_COST (NUMBER), OPTIMIZER_ENV (RAW), OPTIMIZER_ENV_HASH_VALUE (NUMBER), PARSING_USER_ID (NUMBER), PARSING_SCHEMA_ID (NUMBER), PARSING_SCHEMA_NAME (VARCHAR2), KEPT_VERSIONS (NUMBER), ADDRESS (RAW), HASH_VALUE (NUMBER), OLD_HASH_VALUE (NUMBER), PLAN_HASH_VALUE (NUMBER), MODULE (VARCHAR2), MODULE_HASH (NUMBER), ACTION (VARCHAR2), ACTION_HASH (NUMBER), SERIALIZABLE_ABORTS (NUMBER), OUTLINE_CATEGORY (VARCHAR2), CPU_TIME (NUMBER), ELAPSED_TIME (NUMBER), OUTLINE_SID (VARCHAR2), LAST_ACTIVE_CHILD_ADDRESS (RAW), REMOTE (VARCHAR2), OBJECT_STATUS (VARCHAR2), LITERAL_HASH_VALUE (NUMBER), LAST_LOAD_TIME (DATE), IS_OBSOLETE (VARCHAR2), IS_BIND_SENSITIVE (VARCHAR2), IS_BIND_AWARE (VARCHAR2), CHILD_LATCH (NUMBER), SQL_PROFILE (VARCHAR2), SQL_PATCH (VARCHAR2), SQL_PLAN_BASELINE (VARCHAR2), PROGRAM_ID (NUMBER), PROGRAM_LINE# (NUMBER), EXACT_MATCHING_SIGNATURE (NUMBER), FORCE_MATCHING_SIGNATURE (NUMBER), LAST_ACTIVE_TIME (DATE), BIND_DATA (RAW), TYPECHECK_MEM (NUMBER), IO_CELL_OFFLOAD_ELIGIBLE_BYTES (NUMBER), IO_INTERCONNECT_BYTES (NUMBER), PHYSICAL_READ_REQUESTS (NUMBER), PHYSICAL_READ_BYTES (NUMBER), PHYSICAL_WRITE_REQUESTS (NUMBER), PHYSICAL_WRITE_BYTES (NUMBER), OPTIMIZED_PHY_READ_REQUESTS (NUMBER), LOCKED_TOTAL (NUMBER), PINNED_TOTAL (NUMBER), IO_CELL_UNCOMPRESSED_BYTES (NUMBER), IO_CELL_OFFLOAD_RETURNED_BYTES (NUMBER)

**SQL-статистика (v$sqlarea)**: 16200 строк

### Топ SQL по общее время выполнения (ELAPSED_TIME)
| # | SQL_ID | ELAPSED_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0wwyk0pqsnujb | 1515222976960 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 2 | bj9pq2wpng4s5 | 1505384793240 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | 7qhkzabrb48pq | 993790015555 | 168545 | 184943128120 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 3ww9gr940043m | 990546881463 | 164886 | 912537077508 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | b67rmh804b6gq | 852342754880 | 33584 | 37779169389 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 6 | 879vqu6z5y9mr | 838127545908 | 16243 | 322489456486 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 7 | 760jh8rmgwpmm | 741360832187 | 33744 | 142558269574 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | 5d9kaavuzx4n3 | 453720819600 | 123881 | 358351867870 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 9 | g8ksp8wwj63s0 | 239392081814 | 34884 | 234516025526 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPLPC5 | `SELECT "RUL_ID","DAT_PRED","VES_TOT","N_GR_ATT" FROM "CCY"."V_PREDJAVL_1" "T" WHERE NVL("VES_TOT",0)` |
| 10 | 0ms81uztztz10 | 233550035028 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 11 | ckrnv4fdpfqsa | 150193919508 | 326119 | 127373911725 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 12 | fm08f5mh7hnwu | 149334684571 | 168710 | 99391616049 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 13 | bcv4457r4sdvv | 145431159897 | 166923158 | 140967380738 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=158632:14 | SCHEMA=CCY | `SELECT TT.ID_REC, TT.KCEH, TT.BEG_DATE, TT.END_DATE, TT.START_OFFSET, TT.KOL_SMEN, TT.PEREH_BRIG FRO` |
| 14 | 8argb4g1yqckk | 128723876946 | 56242 | 18405698585 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 15 | b8pkjsbatpp4p | 127653183385 | 12464 | 14424419173 | MODULE=None | PROG=2158018:308 | SCHEMA=CCY | `SELECT ID FROM SAP_DIFF_REQUEST T WHERE EXISTS (SELECT 1 FROM SAP_DIFF_RESPONSE R WHERE T.ID = R.REQ` |

### Топ SQL по CPU-времени (CPU_TIME)
| # | SQL_ID | CPU_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0wwyk0pqsnujb | 1051339887865 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 2 | 3ww9gr940043m | 912537077508 | 164886 | 912537077508 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | bj9pq2wpng4s5 | 478068344476 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 4 | 5d9kaavuzx4n3 | 358351867870 | 123881 | 358351867870 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | 879vqu6z5y9mr | 322489456486 | 16243 | 322489456486 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | g8ksp8wwj63s0 | 234516025526 | 34884 | 234516025526 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPLPC5 | `SELECT "RUL_ID","DAT_PRED","VES_TOT","N_GR_ATT" FROM "CCY"."V_PREDJAVL_1" "T" WHERE NVL("VES_TOT",0)` |
| 7 | 7qhkzabrb48pq | 184943128120 | 168545 | 184943128120 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | 760jh8rmgwpmm | 142558269574 | 33744 | 142558269574 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 9 | bcv4457r4sdvv | 140967380738 | 166923158 | 140967380738 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=158632:14 | SCHEMA=CCY | `SELECT TT.ID_REC, TT.KCEH, TT.BEG_DATE, TT.END_DATE, TT.START_OFFSET, TT.KOL_SMEN, TT.PEREH_BRIG FRO` |
| 10 | ckrnv4fdpfqsa | 127373911725 | 326119 | 127373911725 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 11 | fm08f5mh7hnwu | 99391616049 | 168710 | 99391616049 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 12 | 2aamj93xvtanh | 96446965360 | 498 | 96446965360 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=IGSTSPEP | `SELECT "ID_CAMPAIGN","DATE_START","DATE_END","RAL","FAKT","TEOR","PLANT","COLOR","CODE" FROM "V_SUKP` |
| 13 | 4gy6n2pw9k0yv | 78841410151 | 158827 | 78841410151 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPLPC5 | `SELECT "TIP_AGR","RUL_ID","KOD_PARAM","NOM_RAB_ORG","PARAM_MIN","PARAM_MAX","PARAM_SRED","PARAM_OTKL` |
| 14 | 51g9ndsdhrvjc | 68405288271 | 595391 | 68405288271 | MODULE=None | PROG=2058695:1462 | SCHEMA=CP | `SELECT EXTRACTVALUE(VALUE(T), '*/key') AS KEY, NVL(EXTRACTVALUE(VALUE(T), '*/value/ts'), '-') AS TS,` |
| 15 | f0j59cnw7vn7r | 65779446369 | 1011815 | 65779446369 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=HLYSTOVA_LV2 | `SELECT NVL(O.VNOTE , P.NMTT7 )  FROM POZITION N , POZIC P , TH_POZ_O O   WHERE N.ID_POS_ZAK =  :1  A` |

### Топ SQL по количеству исполнений (EXECUTIONS)
| # | SQL_ID | EXECUTIONS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | caqvuskypru2z | 3420272217 | 3420272217 | 13256465107 | MODULE=/nlmk.l3.ccm.pds/get_roll_type | ACTION=GET | PROG=2058695:18 | SCHEMA=CP | `SELECT T.* FROM DOC_INTERCHANGE T WHERE T.ID = :B1 ` |
| 2 | 42wd7pfmkttf7 | 2526229638 | 2526229638 | 30079357430 | MODULE=/nlmk.l3.ccm.pds/get_roll_type | ACTION=GET | PROG=2058694:729 | SCHEMA=CP | `SELECT SUBSTR(STR, INSTR(STR, '/', DECODE(LEVEL, 1, 0, 1), GREATEST(LEVEL - 1, 1)) + 1, DECODE(CONNE` |
| 3 | 6jbxdwtgn05gr | 1347083580 | 1347083580 | 5434145865 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=65702:12 | SCHEMA=CCY | `SELECT * FROM PARAM_RUL WHERE KOD_PARAM=:B2 AND RUL_ID = :B1 ` |
| 4 | ch14wqmd38pza | 764639563 | 764639563 | 9920912833 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=66022:27 | SCHEMA=CCY | `SELECT NVL(TO_CHAR(R.PARAM_SRED),R.PRIZNAK) PARAM_SRED, NVL(I.TIP_ISPIT,88) NTIP, CASE WHEN I.VID_IS` |
| 5 | 4z6jfdhm5uxr7 | 608817612 | 608817612 | 2943120560 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select longname from javasnm$ where short = :1` |
| 6 | 541jsg12xb2wv | 585970795 | 585970795 | 1463302339 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=62107:23 | SCHEMA=STANDART | `SELECT COUNT(ROLE) FROM ROLMEN WHERE IDNEM=:B1 ` |
| 7 | 5q3r38wqvr688 | 575307025 | 575307025 | 1626500069 | MODULE=JDBC Thin Client | ACTION=None | PROG=66087:16 | SCHEMA=CCY | `SELECT R.KOD_REJ FROM NAZN_REJ_SHEM R WHERE R.ID_NAZN=:B2 AND R.TIP_AGR=:B1 ` |
| 8 | dz001qxkq2q0r | 527642724 | 527642724 | 1103929221 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=62107:39 | SCHEMA=STANDART | `SELECT COUNT(ROLE) FROM ROLMEN WHERE IDNEM=:B2 AND ROLE=:B1 ` |
| 9 | byhn6pckfqna5 | 410626432 | 410626432 | 1419740606 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=64510:15 | SCHEMA=CCY | `SELECT PODK_SVOIST.PARAM_SRED FROM SERT_PODKAT,PODK_SVOIST WHERE SERT_PODKAT.PLAV=:B3 AND SERT_PODKA` |
| 10 | 3fvcw33yg1nk1 | 357340247 | 357340247 | 1054308236 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=64439:12 | SCHEMA=CCY | `SELECT S.KODDEF FROM SDEFEKT S WHERE S.ID_DEF=:B1 ` |
| 11 | b2wv9hp2fz3wb | 327079430 | 327079430 | 1051107515 | MODULE=None | PROG=64527:12 | SCHEMA=CCY | `SELECT ST.ID_SOPR_KART FROM STR_SOPROV_KART ST WHERE ST.ID_SOPROVOD=:B1 ` |
| 12 | 9ghu6kr0xm3rs | 304320094 | 304320094 | 521166445 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=64064:13 | SCHEMA=CCY | `SELECT * FROM NCI_TIPPOKR WHERE KODP=:B1 ` |
| 13 | 5pjx5pj5xsmn7 | 268949524 | 268949524 | 1442123658 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=VOSTRIKOV_AS | `SELECT SYSDATE  FROM SYS.DUAL   ` |
| 14 | f89mjn1213t46 | 266241558 | 266241558 | 517566793 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=80555:14 | SCHEMA=CCY | `SELECT SH_UP_CHAR FROM NCI_SHEM_UPAK N WHERE N.ID_SHEM_UP=:B1 ` |
| 15 | 2banzv3ca2bxh | 262717031 | 262717031 | 3396417798 | MODULE=JDBC Thin Client | ACTION=None | PROG=64116:14 | SCHEMA=CCY | `SELECT PARAM_SRED FROM PARAM_RUL WHERE RUL_ID=:B3 AND KOD_PARAM=:B2 AND NOM_RAB_ORG=:B1 ` |

### Топ SQL по буферным чтениям (BUFFER_GETS)
| # | SQL_ID | BUFFER_GETS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 3z2uhq47ptxwg | 3878359799 | 398904 | 9567201157 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=MARKOV_AN | `SELECT MAX (T.IDLABEL) FROM SLABELPRINT T   WHERE T.IDLABSORT =  '0000120170410095802743024'  AND T.` |
| 2 | 1c9sk47090rn8 | 3698753993 | 116200919 | 2593086483 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=77377:30 | SCHEMA=CCY | `SELECT COUNT(*) FROM RUL R WHERE AGR_ID= 36 AND NVL(R.IND_RAZMAT,0)!=-3 START WITH RUL_ID=:B1 CONNEC` |
| 3 | bmdkj5tms52cv | 3698750888 | 116200933 | 2779172566 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=77377:18 | SCHEMA=CCY | `SELECT AGR_ID FROM RUL R WHERE AGR_ID IN (28,29) AND NVL(R.IND_RAZMAT,0)!=-3 START WITH RUL_ID=:B1 C` |
| 4 | fbmcr2kc0jw1r | 3624555596 | 4576 | 24370592960 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 223 | PROG=0:0 | SCHEMA=PETROV_VA | `begin :zak := :zak_in; :pr  := :pr_in;  if :dolg_in = 5 then    :dolg := 4;  elsif  :dolg_in = 4 the` |
| 5 | bj9pq2wpng4s5 | 3394712078 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 6 | byhn6pckfqna5 | 3376551309 | 410626432 | 1419740606 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=64510:15 | SCHEMA=CCY | `SELECT PODK_SVOIST.PARAM_SRED FROM SERT_PODKAT,PODK_SVOIST WHERE SERT_PODKAT.PLAV=:B3 AND SERT_PODKA` |
| 7 | 19wdcysuw7rum | 3337751540 | 322875 | 3398818797 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | atjv56twzak0d | 3275010618 | 82651 | 1279971832 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=66320:75 | SCHEMA=CCY | `SELECT MAX(PARAM_SRED) FROM PARAM_RUL P, RUL R, RUL R1 WHERE P.RUL_ID = R.RUL_ID AND P.KOD_PARAM = 1` |
| 9 | 9c9433ft0mcuy | 3273722367 | 14133897 | 6871397717 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select  /*+ INDEX(TAB AQ$_AQ_OBJEVENT_TABLE_I) */   tab.rowid, tab.msgid, tab.corrid, tab.priority, ` |
| 10 | 2aamj93xvtanh | 3248793556 | 498 | 96446965360 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=IGSTSPEP | `SELECT "ID_CAMPAIGN","DATE_START","DATE_END","RAL","FAKT","TEOR","PLANT","COLOR","CODE" FROM "V_SUKP` |
| 11 | fm08f5mh7hnwu | 3201091081 | 168710 | 99391616049 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 12 | ch14wqmd38pza | 3041205549 | 764639563 | 9920912833 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=66022:27 | SCHEMA=CCY | `SELECT NVL(TO_CHAR(R.PARAM_SRED),R.PRIZNAK) PARAM_SRED, NVL(I.TIP_ISPIT,88) NTIP, CASE WHEN I.VID_IS` |
| 13 | fwqy2zmyu280z | 2971939184 | 4313 | 9585910962 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SAMARINA_YV | `SELECT RUL_ID,DAT,N_SMEN,AGR_ID,nplv,nprt,godpr,GOD_PRT,KCEH,ID_POS_ZAK,MARK,PRT,NSNZ,GDIS,NPOZ,MARK` |
| 14 | 4gy6n2pw9k0yv | 2927356407 | 158827 | 78841410151 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SUPLPC5 | `SELECT "TIP_AGR","RUL_ID","KOD_PARAM","NOM_RAB_ORG","PARAM_MIN","PARAM_MAX","PARAM_SRED","PARAM_OTKL` |
| 15 | bcd8bxf03yuk3 | 2925780084 | 55588971 | 1776857867 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=113422:17 | SCHEMA=CCY | `SELECT P.PARAM_SRED FROM RUL R,( SELECT V.RUL_ID FROM RUL V CONNECT BY PRIOR RUL_ID_PARENT = RUL_ID ` |

### Топ SQL по дисковым чтениям (DISK_READS)
| # | SQL_ID | DISK_READS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | b8pkjsbatpp4p | 4172240210 | 12464 | 14424419173 | MODULE=None | PROG=2158018:308 | SCHEMA=CCY | `SELECT ID FROM SAP_DIFF_REQUEST T WHERE EXISTS (SELECT 1 FROM SAP_DIFF_RESPONSE R WHERE T.ID = R.REQ` |
| 2 | 760jh8rmgwpmm | 3216297036 | 33744 | 142558269574 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | c32tsr5kdphzx | 1980326281 | 49875 | 8161935090 | MODULE=/nlmk.l3.ccm.pds/create_label | ACTION=POST | PROG=0:0 | SCHEMA=CCY | `begin   pkg_ords_response_label.process(:1 ); end;` |
| 4 | 7qhkzabrb48pq | 1679842167 | 168545 | 184943128120 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | b67rmh804b6gq | 1232636754 | 33584 | 37779169389 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 6 | 0wwyk0pqsnujb | 1173885246 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 7 | 0ms81uztztz10 | 927105302 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 8 | bj9pq2wpng4s5 | 900789830 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 9 | b4g45h0xnmvm3 | 829560577 | 344 | 14270141273 | MODULE=None | PROG=2361853:176 | SCHEMA=L2COMMON | `begin pm1400_calc.calc_outcart(:1); end;` |
| 10 | 8v8wz685an1cp | 637328779 | 168715 | 16694480392 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 11 | b9xgm5quv8np7 | 495552932 | 4986 | 2851793660 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=ARM_SHIPMENT | `select distinct cdscertifi0_.nsrt as nsrt1_23_0_, cdscertifi0_.god_sert as god_sert2_23_0_, cdscerti` |
| 12 | 5uq66s78c42qx | 390696314 | 3931 | 2258661453 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=ARM_SHIPMENT | `select distinct cdscertifi0_.nsrt as nsrt1_23_0_, cdscertifi0_.god_sert as god_sert2_23_0_, cdscerti` |
| 13 | 8m32w35mh12mg | 333251998 | 306 | 6576205022 | MODULE=None | PROG=148876:35 | SCHEMA=L2COMMON | `merge into measurements d                        using (select :1 as munit_cid, attr_id, 0 as msr_ty` |
| 14 | 3ww9gr940043m | 276138891 | 164886 | 912537077508 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 15 | 42fxyfps33v9v | 275639369 | 5125 | 3581452864 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=ZYUZIN_IS | `SELECT ROWID,ID_SOPROVOD,NOM_SOPROVOD,GOD_SOPROVOD,VID_ISPIT,NVID_ISPIT,KOD_LAB,NAME,AGR_ID,AGR_NAME` |

### Топ SQL по времени ожидания приложения (APPLICATION_WAIT_TIME)
| # | SQL_ID | APPLICATION_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0wwyk0pqsnujb | 74125746424 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 2 | 879vqu6z5y9mr | 22588706279 | 16243 | 322489456486 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | 2u65ju0fx7ahn | 14949586396 | 32208 | 1633556496 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `BEGIN pcom_cart_api_l25.setIncartStatus(:1 , :2 , :3 , :4 , :5 , :6 , :7 , :8 , :9 , :10 , :11 , :12` |
| 4 | fwyvaxpmbhwtm | 10759329154 | 42557 | 3419465455 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `BEGIN pcom_cart_api_l25.setOutcartStatus(:1 , :2 , :3 , :4 , :5 , :6 , :7 , :8 , :9 , :10 , :11 , :1` |
| 5 | 5rfsyyrqwp1z1 | 9509459471 | 227 | 7976373608 | MODULE=DBMS_SCHEDULER | ACTION=M1400_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 6 | bj9pq2wpng4s5 | 8407039599 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 7 | 0ms81uztztz10 | 7890757966 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 8 | 1q3a8x0zn3h8y | 6176363728 | 202 | 13581467702 | MODULE=DBMS_SCHEDULER | ACTION=APP3_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 9 | fsu8m20jny7tq | 5865340846 | 881 | 16031885 | MODULE=None | PROG=2361853:176 | SCHEMA=L2COMMON | `begin pm1551.calc_outcart(:1); end;` |
| 10 | 73guautr8q3nk | 4890742329 | 15521 | 1988839387 | MODULE=M1400_PROC | ACTION=Event Processing | PROG=72778:561 | SCHEMA=L2COMMON | `begin pm1400_proc.processOutputCoilStatus; end;` |

### Топ SQL по времени ожидания конкурентности (CONCURRENCY_WAIT_TIME)
| # | SQL_ID | CONCURRENCY_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 879vqu6z5y9mr | 3505277146 | 16243 | 322489456486 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | 0wwyk0pqsnujb | 2874164273 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | bj9pq2wpng4s5 | 2246107950 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 4 | 5rfsyyrqwp1z1 | 2156309922 | 227 | 7976373608 | MODULE=DBMS_SCHEDULER | ACTION=M1400_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 5 | 72uxhj54n4qak | 1656275439 | 57490 | 11905359999 | MODULE=None | PROG=0:0 | SCHEMA=CCY | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 6 | 7jp3dr0hxgrgk | 1454555794 | 121 | 999131311 | MODULE=DBMS_SCHEDULER | ACTION=RM1_PROC | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job;  next_date TIMESTAMP WITH TIME ZONE := :mydate;  broken BOOLEAN ` |
| 7 | 4vs91dcv7u1p6 | 1338538069 | 15158499 | 1983750730 | MODULE=oracle@nl-sp-ccm4-db1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SYS | `insert into sys.aud$( sessionid,entryid,statement,ntimestamp#, userid,userhost,terminal,action#,retu` |
| 8 | 73guautr8q3nk | 1197286905 | 15521 | 1988839387 | MODULE=M1400_PROC | ACTION=Event Processing | PROG=72778:561 | SCHEMA=L2COMMON | `begin pm1400_proc.processOutputCoilStatus; end;` |
| 9 | 2u65ju0fx7ahn | 1027975147 | 32208 | 1633556496 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=L2COMMON | `BEGIN pcom_cart_api_l25.setIncartStatus(:1 , :2 , :3 , :4 , :5 , :6 , :7 , :8 , :9 , :10 , :11 , :12` |
| 10 | ankyttqyhf43t | 964983684 | 6625642 | 2233443004 | MODULE=None | PROG=0:0 | SCHEMA=TIGER | `DECLARE   uu VARCHAR2(100);   hh VARCHAR2(200);   xhh VARCHAR2(200) := '-';   i NUMBER := 0;   j NUM` |

### Топ SQL по времени ожидания кластера (CLUSTER_WAIT_TIME)
| # | SQL_ID | CLUSTER_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | gmk015h8jc003 | 0 | 352017 | 2510825 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 223 | PROG=80803:51 | SCHEMA=CCY | `SELECT COUNT(*) FROM EDETSER E, RUL R WHERE E.NSRT = :B3 AND E.GOD_SERT = :B2 AND E.NSTR = :B1 AND R` |
| 2 | ds9qvbgrtw00c | 0 | 25736 | 623096 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=DEMIDOVA_EO | `SELECT COL , TATR FROM ATRS   WHERE IDOBJ =  :1   ` |
| 3 | ftr2jkvanh00d | 0 | 1 | 1265 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SAP_PO_ZR_IN_06 | `update CCY.MANAGEMENT_CREDIT set cmgst = 'A' where deliv_numb = '0925695598'` |
| 4 | 1kb95r9u5c00q | 0 | 1331 | 172736 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=2281679:26 | SCHEMA=CCY | `SELECT COUNT(DISTINCT R.RUL_ID) FROM RUL R WHERE R.AKTUAL<>-1 AND R.IND_RAZMAT IS NULL AND (R.IND_ZA` |
| 5 | 5pnk3z99yw014 | 0 | 1 | 1797 | MODULE=oracle@nl-sp-sbyt-db1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=U | `UPDATE "TH_POZ_O" "A1" SET ("VNOTE","PRC","NAMELST","IDS")= (SELECT "A3"."VNOTE","A3"."PRC","A3"."NA` |
| 6 | 9nnb0q6wsc019 | 0 | 1 | 439 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SAP_PO_ZR_IN_06 | `update CCY.MANAGEMENT_CREDIT set cmgst = ' ' where deliv_numb = '0925691553'` |
| 7 | 5uq2v4hqgw01b | 0 | 2 | 16064 | MODULE=plsql2svn.exe | ACTION=None | PROG=5028:2688 | SCHEMA=CP | `SELECT /*+all_rows*/ VALUE(KU$), 0 FROM SYS.KU$_OBJGRANT_VIEW KU$ WHERE NOT BITAND(KU$.BASE_OBJ.FLAG` |
| 8 | 60d2cnz08001g | 0 | 1 | 840 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SAP_PO_ZR_IN_06 | `update CCY.MANAGEMENT_CREDIT set cmgst = 'A' where deliv_numb = '0925695581'` |
| 9 | 7ac1754am001k | 0 | 2 | 0 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=98536:182 | SCHEMA=CCY | `INSERT INTO PI_MES.MES_PRZ_ITEMS_IN@PI_LINK.NLMK (PRZNUM, CON_MATR_TYPE, MATNR, ATTR_CODE, VAL_NUM, ` |
| 10 | 87xua46kus01y | 0 | 11257 | 311748 | MODULE=None | PROG=0:0 | SCHEMA=TPAR | `insert into data16 (meter_time, agr_id, v, nataj_nach, udlin_prav, nataj_prav, glubina_1, glubina_2)` |

### Топ SQL по времени ожидания пользовательского IO (USER_IO_WAIT_TIME)
| # | SQL_ID | USER_IO_WAIT_TIME | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | bj9pq2wpng4s5 | 969335986812 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 2 | b67rmh804b6gq | 822818446349 | 33584 | 37779169389 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | 7qhkzabrb48pq | 808318031310 | 168545 | 184943128120 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 4 | 760jh8rmgwpmm | 602271146927 | 33744 | 142558269574 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | 0wwyk0pqsnujb | 374987755370 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 6 | 0ms81uztztz10 | 179596278953 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 7 | b8pkjsbatpp4p | 113249753913 | 12464 | 14424419173 | MODULE=None | PROG=2158018:308 | SCHEMA=CCY | `SELECT ID FROM SAP_DIFF_REQUEST T WHERE EXISTS (SELECT 1 FROM SAP_DIFF_RESPONSE R WHERE T.ID = R.REQ` |
| 8 | c32tsr5kdphzx | 74436018205 | 49875 | 8161935090 | MODULE=/nlmk.l3.ccm.pds/create_label | ACTION=POST | PROG=0:0 | SCHEMA=CCY | `begin   pkg_ords_response_label.process(:1 ); end;` |
| 9 | 5d9kaavuzx4n3 | 60856330131 | 123881 | 358351867870 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 10 | 9wsqsgb6fup70 | 57118678986 | 720 | 3728918368 | MODULE=None | PROG=2155908:5 | SCHEMA=CCY | `SELECT COUNT(1) FROM PODKAT P, RUL R WHERE TRUNC(P.DATOBR) >= TO_DATE('01.01.2022', 'dd.mm.yyyy') AN` |

### Топ SQL по физическим чтениям в байтах (PHYSICAL_READ_BYTES)
| # | SQL_ID | PHYSICAL_READ_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 7qhkzabrb48pq | 330420615831552 | 168545 | 184943128120 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | bj9pq2wpng4s5 | 324038619086848 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | c32tsr5kdphzx | 51407204982784 | 49875 | 8161935090 | MODULE=/nlmk.l3.ccm.pds/create_label | ACTION=POST | PROG=0:0 | SCHEMA=CCY | `begin   pkg_ords_response_label.process(:1 ); end;` |
| 4 | b8pkjsbatpp4p | 34178991800320 | 12464 | 14424419173 | MODULE=None | PROG=2158018:308 | SCHEMA=CCY | `SELECT ID FROM SAP_DIFF_REQUEST T WHERE EXISTS (SELECT 1 FROM SAP_DIFF_RESPONSE R WHERE T.ID = R.REQ` |
| 5 | 760jh8rmgwpmm | 26347905318912 | 33744 | 142558269574 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | b67rmh804b6gq | 10097760288768 | 33584 | 37779169389 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 7 | 0wwyk0pqsnujb | 9616467935232 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 8 | 0ms81uztztz10 | 7594846633984 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 9 | b4g45h0xnmvm3 | 6795760246784 | 344 | 14270141273 | MODULE=None | PROG=2361853:176 | SCHEMA=L2COMMON | `begin pm1400_calc.calc_outcart(:1); end;` |
| 10 | 8v8wz685an1cp | 5220997357568 | 168715 | 16694480392 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по физическим записям в байтах (PHYSICAL_WRITE_BYTES)
| # | SQL_ID | PHYSICAL_WRITE_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0wwyk0pqsnujb | 6017774206976 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 2 | 0ms81uztztz10 | 156180512768 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | csnh2ry4kdkgr | 113625931776 | 148738 | 6134164614 | MODULE=None | PROG=2363986:71 | SCHEMA=L2COMMON | `WITH PAIRS AS (SELECT RTIME AS T1, ENC_VALUE AS X1, LEAD(RTIME) OVER(ORDER BY RTIME) AS T2, LEAD(ENC` |
| 4 | 5d9kaavuzx4n3 | 88745918464 | 123881 | 358351867870 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | gad00qh3va5wj | 87274258432 | 1033 | 5001654604 | MODULE=PL/SQL Developer | ACTION=Script for L2COMMON.PCALC_MANAGE | PROG=2361853:176 | SCHEMA=L2COMMON | `begin pcom_calc_int.calc_outcart_intervals(:1); end;` |
| 6 | cf3w4x7ktqwxp | 63743426560 | 769 | 19300335540 | MODULE=None | PROG=2361853:176 | SCHEMA=L2COMMON | `begin pcom_calc_points.calc_outcart(:1); end;` |
| 7 | c299w62ymt4c7 | 60780552192 | 614 | 17591651981 | MODULE=None | PROG=2148596:12 | SCHEMA=L2COMMON | `MERGE INTO MEASUREMENTS D USING (SELECT OUTCART_CID AS MUNIT_CID, ATTR_ID, 0 AS MSR_TYPE, 0 AS IDX, ` |
| 8 | ckrnv4fdpfqsa | 32182640640 | 326119 | 127373911725 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 9 | 9z6u9zs9zkc2u | 24649277440 | 1430 | 1346847692 | MODULE=PL/SQL Developer | ACTION=Script for L2COMMON.PCALC_MANAGE | PROG=2361853:176 | SCHEMA=L2COMMON | `begin pcom_calc_int.calc_incart_intervals(:1); end;` |
| 10 | 1zzb3w13n429x | 9286443008 | 15 | 101577666 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=BUDKIN_AY | `SELECT ROWID,ID_ISPIT,ID_SOPROVOD,DAT_ISPIT,NOM_SOPROVOD,GOD_SOPROVOD,VID_ISPIT,NVID_ISPIT,KOD_LAB,N` |

### Топ SQL по байтам IO через interconnect (IO_INTERCONNECT_BYTES)
| # | SQL_ID | IO_INTERCONNECT_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 7qhkzabrb48pq | 330422270066688 | 168545 | 184943128120 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 2 | bj9pq2wpng4s5 | 324038619086848 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 3 | c32tsr5kdphzx | 51407204982784 | 49875 | 8161935090 | MODULE=/nlmk.l3.ccm.pds/create_label | ACTION=POST | PROG=0:0 | SCHEMA=CCY | `begin   pkg_ords_response_label.process(:1 ); end;` |
| 4 | b8pkjsbatpp4p | 34178991800320 | 12464 | 14424419173 | MODULE=None | PROG=2158018:308 | SCHEMA=CCY | `SELECT ID FROM SAP_DIFF_REQUEST T WHERE EXISTS (SELECT 1 FROM SAP_DIFF_RESPONSE R WHERE T.ID = R.REQ` |
| 5 | 760jh8rmgwpmm | 26347905318912 | 33744 | 142558269574 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | 0wwyk0pqsnujb | 15634242142208 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 7 | b67rmh804b6gq | 10098914525184 | 33584 | 37779169389 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 8 | 0ms81uztztz10 | 7751027146752 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 9 | b4g45h0xnmvm3 | 6795760246784 | 344 | 14270141273 | MODULE=None | PROG=2361853:176 | SCHEMA=L2COMMON | `begin pm1400_calc.calc_outcart(:1); end;` |
| 10 | 8v8wz685an1cp | 5221977284608 | 168715 | 16694480392 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по байтам, подходящим под offload (IO_CELL_OFFLOAD_ELIGIBLE_BYTES)
| # | SQL_ID | IO_CELL_OFFLOAD_ELIGIBLE_BYTES | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | gmk015h8jc003 | 0 | 352017 | 2510825 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 223 | PROG=80803:51 | SCHEMA=CCY | `SELECT COUNT(*) FROM EDETSER E, RUL R WHERE E.NSRT = :B3 AND E.GOD_SERT = :B2 AND E.NSTR = :B1 AND R` |
| 2 | ds9qvbgrtw00c | 0 | 25736 | 623096 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=DEMIDOVA_EO | `SELECT COL , TATR FROM ATRS   WHERE IDOBJ =  :1   ` |
| 3 | ftr2jkvanh00d | 0 | 1 | 1265 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SAP_PO_ZR_IN_06 | `update CCY.MANAGEMENT_CREDIT set cmgst = 'A' where deliv_numb = '0925695598'` |
| 4 | 1kb95r9u5c00q | 0 | 1331 | 172736 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=2281679:26 | SCHEMA=CCY | `SELECT COUNT(DISTINCT R.RUL_ID) FROM RUL R WHERE R.AKTUAL<>-1 AND R.IND_RAZMAT IS NULL AND (R.IND_ZA` |
| 5 | 5pnk3z99yw014 | 0 | 1 | 1797 | MODULE=oracle@nl-sp-sbyt-db1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=U | `UPDATE "TH_POZ_O" "A1" SET ("VNOTE","PRC","NAMELST","IDS")= (SELECT "A3"."VNOTE","A3"."PRC","A3"."NA` |
| 6 | 9nnb0q6wsc019 | 0 | 1 | 439 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SAP_PO_ZR_IN_06 | `update CCY.MANAGEMENT_CREDIT set cmgst = ' ' where deliv_numb = '0925691553'` |
| 7 | 5uq2v4hqgw01b | 0 | 2 | 16064 | MODULE=plsql2svn.exe | ACTION=None | PROG=5028:2688 | SCHEMA=CP | `SELECT /*+all_rows*/ VALUE(KU$), 0 FROM SYS.KU$_OBJGRANT_VIEW KU$ WHERE NOT BITAND(KU$.BASE_OBJ.FLAG` |
| 8 | 60d2cnz08001g | 0 | 1 | 840 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SAP_PO_ZR_IN_06 | `update CCY.MANAGEMENT_CREDIT set cmgst = 'A' where deliv_numb = '0925695581'` |
| 9 | 7ac1754am001k | 0 | 2 | 0 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=98536:182 | SCHEMA=CCY | `INSERT INTO PI_MES.MES_PRZ_ITEMS_IN@PI_LINK.NLMK (PRZNUM, CON_MATR_TYPE, MATNR, ATTR_CODE, VAL_NUM, ` |
| 10 | 87xua46kus01y | 0 | 11257 | 311748 | MODULE=None | PROG=0:0 | SCHEMA=TPAR | `insert into data16 (meter_time, agr_id, v, nataj_nach, udlin_prav, nataj_prav, glubina_1, glubina_2)` |

### Топ SQL по числу физических чтений (PHYSICAL_READ_REQUESTS)
| # | SQL_ID | PHYSICAL_READ_REQUESTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | b67rmh804b6gq | 1231917368 | 33584 | 37779169389 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 2 | 760jh8rmgwpmm | 920828393 | 33744 | 142558269574 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | bj9pq2wpng4s5 | 750584551 | 13111 | 478068344476 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 4 | 7qhkzabrb48pq | 415654587 | 168545 | 184943128120 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 5 | 0wwyk0pqsnujb | 399543888 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 6 | 0ms81uztztz10 | 133430683 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 7 | dkg56jfbs49pt | 73212635 | 168691 | 1782054138 | MODULE=zabbix_proxy: odbc poller #12 [got 0 values in 0 | ACTION=None | PROG=0:0 | SCHEMA=ZABBIX | `SELECT  tablespace_name AS tablespace,  contents FROM DBA_TABLESPACES` |
| 8 | fbmcr2kc0jw1r | 55486222 | 4576 | 24370592960 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 223 | PROG=0:0 | SCHEMA=PETROV_VA | `begin :zak := :zak_in; :pr  := :pr_in;  if :dolg_in = 5 then    :dolg := 4;  elsif  :dolg_in = 4 the` |
| 9 | c32tsr5kdphzx | 55425753 | 49875 | 8161935090 | MODULE=/nlmk.l3.ccm.pds/create_label | ACTION=POST | PROG=0:0 | SCHEMA=CCY | `begin   pkg_ords_response_label.process(:1 ); end;` |
| 10 | 8argb4g1yqckk | 42374897 | 56242 | 18405698585 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по числу физических записей (PHYSICAL_WRITE_REQUESTS)
| # | SQL_ID | PHYSICAL_WRITE_REQUESTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 0wwyk0pqsnujb | 34439286 | 32801 | 1051339887865 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 2 | 5d9kaavuzx4n3 | 8909632 | 123881 | 358351867870 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 3 | ckrnv4fdpfqsa | 3928545 | 326119 | 127373911725 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 4 | 0ms81uztztz10 | 723227 | 231 | 41909830715 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN begin` |
| 5 | bvgc8ubryauhv | 599013 | 66292 | 18466866877 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 6 | csnh2ry4kdkgr | 518681 | 148738 | 6134164614 | MODULE=None | PROG=2363986:71 | SCHEMA=L2COMMON | `WITH PAIRS AS (SELECT RTIME AS T1, ENC_VALUE AS X1, LEAD(RTIME) OVER(ORDER BY RTIME) AS T2, LEAD(ENC` |
| 7 | 879vqu6z5y9mr | 469953 | 16243 | 322489456486 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 8 | gad00qh3va5wj | 392923 | 1033 | 5001654604 | MODULE=PL/SQL Developer | ACTION=Script for L2COMMON.PCALC_MANAGE | PROG=2361853:176 | SCHEMA=L2COMMON | `begin pcom_calc_int.calc_outcart_intervals(:1); end;` |
| 9 | fm08f5mh7hnwu | 347866 | 168710 | 99391616049 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |
| 10 | 6r3kxg5azb4ft | 308566 | 400 | 27317266541 | MODULE=None | PROG=0:0 | SCHEMA=TM | `DECLARE job BINARY_INTEGER := :job; next_date DATE := :mydate;  broken BOOLEAN := FALSE; BEGIN  /*Вр` |

### Топ SQL по обработанным строкам (ROWS_PROCESSED)
| # | SQL_ID | ROWS_PROCESSED | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | caqvuskypru2z | 3420232296 | 3420272217 | 13256465107 | MODULE=/nlmk.l3.ccm.pds/get_roll_type | ACTION=GET | PROG=2058695:18 | SCHEMA=CP | `SELECT T.* FROM DOC_INTERCHANGE T WHERE T.ID = :B1 ` |
| 2 | 42wd7pfmkttf7 | 2837076114 | 2526229638 | 30079357430 | MODULE=/nlmk.l3.ccm.pds/get_roll_type | ACTION=GET | PROG=2058694:729 | SCHEMA=CP | `SELECT SUBSTR(STR, INSTR(STR, '/', DECODE(LEVEL, 1, 0, 1), GREATEST(LEVEL - 1, 1)) + 1, DECODE(CONNE` |
| 3 | 611m7g6t7zm10 | 1216522892 | 13257304 | 4679248616 | MODULE=None | PROG=163909:278 | SCHEMA=TM | `SELECT T.* FROM TM_T_TASKS T, TM_THR_GROUPS TG WHERE T.GROUP_ID = TG.GROUP_ID AND (T.COUNT_EXEC = 0 ` |
| 4 | ch14wqmd38pza | 802240378 | 764639563 | 9920912833 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=66022:27 | SCHEMA=CCY | `SELECT NVL(TO_CHAR(R.PARAM_SRED),R.PRIZNAK) PARAM_SRED, NVL(I.TIP_ISPIT,88) NTIP, CASE WHEN I.VID_IS` |
| 5 | 6jbxdwtgn05gr | 656163063 | 1347083580 | 5434145865 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=65702:12 | SCHEMA=CCY | `SELECT * FROM PARAM_RUL WHERE KOD_PARAM=:B2 AND RUL_ID = :B1 ` |
| 6 | 4s71mb2gjbbkt | 618716461 | 21057794 | 728455454 | MODULE=None | PROG=2058694:1014 | SCHEMA=CP | `SELECT * FROM DOC_INTERCHANGE_ATTR WHERE TAG_ID = :B1 ORDER BY CODE` |
| 7 | 4z6jfdhm5uxr7 | 600513358 | 608817612 | 2943120560 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select longname from javasnm$ where short = :1` |
| 8 | 541jsg12xb2wv | 585970656 | 585970795 | 1463302339 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=62107:23 | SCHEMA=STANDART | `SELECT COUNT(ROLE) FROM ROLMEN WHERE IDNEM=:B1 ` |
| 9 | dz001qxkq2q0r | 527642634 | 527642724 | 1103929221 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=62107:39 | SCHEMA=STANDART | `SELECT COUNT(ROLE) FROM ROLMEN WHERE IDNEM=:B2 AND ROLE=:B1 ` |
| 10 | byhn6pckfqna5 | 406154047 | 410626432 | 1419740606 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=64510:15 | SCHEMA=CCY | `SELECT PODK_SVOIST.PARAM_SRED FROM SERT_PODKAT,PODK_SVOIST WHERE SERT_PODKAT.PLAV=:B3 AND SERT_PODKA` |

### Топ SQL по сортировкам (SORTS)
| # | SQL_ID | SORTS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | ch14wqmd38pza | 764641010 | 764639563 | 9920912833 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=66022:27 | SCHEMA=CCY | `SELECT NVL(TO_CHAR(R.PARAM_SRED),R.PRIZNAK) PARAM_SRED, NVL(I.TIP_ISPIT,88) NTIP, CASE WHEN I.VID_IS` |
| 2 | 0c1g1mhxhm5ap | 60425034 | 60425034 | 439689650 | MODULE=oracle@nl-sp-mes-db3 (TNS V1-V3) | ACTION=None | PROG=111525:12 | SCHEMA=CCY | `SELECT * FROM PAINTPART_TUN_OPER T WHERE T.ID_PART = :B2 AND T.MARK_CASK = :B1 ORDER BY T.OPER_DATE` |
| 3 | g8uh4g25bsccv | 45969864 | 45969821 | 204784834 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=64545:16 | SCHEMA=CCY | `SELECT S.ID_REESTR_AKT FROM REESTR_AKT R, REESTR_AKT_STR S WHERE S.IDSAKT = :B2 AND S.ID_REESTR_AKT ` |
| 4 | aycgyg7jp3znw | 42929918 | 42929908 | 211789891 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=64551:19 | SCHEMA=CCY | `SELECT S.ID_REESTR_AKT FROM REESTR_AKT R, REESTR_AKT_STR S WHERE S.IDSAKT = :B4 AND S.ID_REESTR_AKT ` |
| 5 | 0ws7ahf1d78qa | 39512428 | 19754093 | 2206151091 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select SYS_CONTEXT('USERENV', 'SERVER_HOST'), SYS_CONTEXT('USERENV', 'DB_UNIQUE_NAME'), SYS_CONTEXT(` |
| 6 | 098wqutg2pmub | 33907520 | 33907501 | 427501756 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=64333:11 | SCHEMA=CCY | `SELECT A.NOM_AKT,A.DAT_AKT, GET_KOD_DEFECT_ID(A.KODDEF) KODDEF, RTRIM(SUBSTR(N.NVIN,1,10)) NAME_VIN,` |
| 7 | 42sqnu40zhkrp | 33165033 | 33165033 | 291549636 | MODULE=None | PROG=2053016:31 | SCHEMA=CCY | `SELECT T.*,I.TIP_ISPIT FROM REZ_ISPIT T, ISP_PRT I WHERE KOD_PARAM=:B4 AND PRT_NASL=:B3 AND GOD_PRT_` |
| 8 | dqgbv2uv1frk3 | 22012163 | 22012163 | 781719743 | MODULE=JDBC Thin Client | ACTION=None | PROG=65697:23 | SCHEMA=CCY | `SELECT TIP_AGR FROM CALC_NZP_DETAIL D, MARK_NAZN_CALC M WHERE D.PRAV_ID=:B2 AND D.TIP_AGR IS NOT NUL` |
| 9 | 4s71mb2gjbbkt | 21057797 | 21057794 | 728455454 | MODULE=None | PROG=2058694:1014 | SCHEMA=CP | `SELECT * FROM DOC_INTERCHANGE_ATTR WHERE TAG_ID = :B1 ORDER BY CODE` |
| 10 | 7zm3njw7v996q | 15725410 | 15030404 | 94082864 | MODULE=None | PROG=2053016:42 | SCHEMA=CCY | `SELECT T.TIP_ISPIT,T.NUM_REZ PARAM_SRED FROM V_REZ_ISP_CALC T WHERE T.KOD_PARAM=:B4 AND T.PRT=:B3 AN` |

### Топ SQL по числу парсингов (PARSE_CALLS)
| # | SQL_ID | PARSE_CALLS | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | cm5vu20fhtnq1 | 96643082 | 96550179 | 2771909855 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select /*+ connect_by_filtering */ privilege#,level from sysauth$ connect by grantee#=prior privileg` |
| 2 | fk9vwag6bt63f | 53364896 | 53295494 | 28115980209 | MODULE=oracle@nl-sp-ccm4-db1 (TNS V1-V3) | ACTION=None | PROG=0:10 | SCHEMA=SYS | `SELECT S.SID, S.SERIAL#, P.SPID, S.OSUSER, S.MACHINE, S.PROGRAM FROM V$SESSION S, V$PROCESS P WHERE ` |
| 3 | crzykq8s5g5cj | 28841513 | 28859080 | 282946209 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SAMSONOV_MY | `SELECT PARAM FROM NCI_SVOISTVO   WHERE KOD_PARAM =  :1   ` |
| 4 | 9gy0jcg0ggthv | 28185237 | 147507996 | 22552261950 | MODULE=JDBC Thin Client | ACTION=None | PROG=114101:14 | SCHEMA=CCY | `SELECT T.USERNAME,T.OSUSER,T.TERMINAL,T.MODULE,T.PROGRAM FROM SYS.V_$SESSION T WHERE AUDSID = SYS_CO` |
| 5 | 1v717nvrhgbn9 | 27560943 | 57237778 | 529458591 | MODULE=None | PROG=649:180 | SCHEMA=TIGER | `SELECT USER FROM SYS.DUAL` |
| 6 | 459f3z9u4fb3u | 21062724 | 21063758 | 1679510518 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select value$ from props$ where name = 'GLOBAL_DB_NAME'` |
| 7 | 0ws7ahf1d78qa | 19751119 | 19754093 | 2206151091 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select SYS_CONTEXT('USERENV', 'SERVER_HOST'), SYS_CONTEXT('USERENV', 'DB_UNIQUE_NAME'), SYS_CONTEXT(` |
| 8 | 5ur69atw3vfhj | 19711853 | 19713461 | 882405071 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select decode(failover_method, NULL, 0 , 'BASIC', 1, 'PRECONNECT', 2 , 'PREPARSE', 4 , 0), decode(fa` |
| 9 | 38qaqzc0tc3rb | 16762242 | 113511508 | 952729952 | MODULE=JDBC Thin Client | ACTION=None | PROG=114101:45 | SCHEMA=CCY | `SELECT COUNT(*) FROM NCI_AUDIT_APP WHERE TABNAME = :B2 AND ELMENU = :B1 AND COL_NAME = '*NEVER*' ` |
| 10 | 4vs91dcv7u1p6 | 15147818 | 15158499 | 1983750730 | MODULE=oracle@nl-sp-ccm4-db1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=SYS | `insert into sys.aud$( sessionid,entryid,statement,ntimestamp#, userid,userhost,terminal,action#,retu` |

### Топ SQL по занимаемой памяти в пуле (SHARABLE_MEM)
| # | SQL_ID | SHARABLE_MEM | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 4yz26mub7p6a9 | 72910116 | 4333 | 324544318 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=LEVIN_SV | `SELECT PARAM_CODE,PARAM_SUBCODE,PARAM_AVERAGE,AGR_ID FROM V_AGR_PARAM_PDS WHERE (AGR_ID=:1)` |
| 2 | 9jk5afyzdrcht | 23872493 | 138 | 2977708 | MODULE=SQL*Plus | ACTION=None | PROG=5028:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+rule*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('M_VIEW_T', '7')), KU$.MVIEW_TAB.OBJ` |
| 3 | 98wu2qq5ru3p4 | 22150791 | 33 | 1961348 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=DEMIDOVA_EO | `/*    П О Л И М Е Р Ы    */ with base as (   select r.rul_id,          r.prt,          r.god_prt   f` |
| 4 | 48s82gxsc2bfq | 20733849 | 40 | 3011734 | MODULE=SQL*Plus | ACTION=None | PROG=5028:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+all_rows*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('TABLE_T', '7')), KU$.OBJ_NUM FR` |
| 5 | ggu8xspju1vvh | 20615977 | 140 | 3149535 | MODULE=SQL*Plus | ACTION=None | PROG=5028:2693 | SCHEMA=EXPORT_SOURCE | `SELECT /*+all_rows*/ SYS_XMLGEN(VALUE(KU$), XMLFORMAT.createFormat2('TABLE_T', '7')), KU$.OBJ_NUM FR` |
| 6 | 4p8nk0q3ddkhz | 11624708 | 4515 | 6006881 | MODULE=None | PROG=2293383:7 | SCHEMA=CCY | `WITH W AS (SELECT CONNECT_BY_ROOT CHAIN_ID AS ROOT_CHAIN_ID, W.CHAIN_ID, ROWNUM N, LEVEL L, W.SID, W` |
| 7 | fn4tbqkw107sr | 9397645 | 244414 | 29818798 | MODULE=None | PROG=0:0 | SCHEMA=PLAN_BASE | `select t.outputmaterialid, t.seqnr, t.stockingpointid, p.vid_product, p.standardproductid, p.orderco` |
| 8 | 7xb4axhqds1fb | 7806808 | 2767 | 466304934 | MODULE=zabbix_proxy: odbc poller #8 [got 0 values in 0. | ACTION=None | PROG=0:0 | SCHEMA=ZABBIX | `SELECT 'SYS::' || METRIC_NAME AS METRIC, ROUND(VALUE,3) as VALUE   FROM GV$SYSMETRIC WHERE GROUP_ID ` |
| 9 | 9rt5s3u69wwfa | 7346306 | 116 | 644907 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=142172:44 | SCHEMA=CCY | `SELECT MUNIT_ID,E_DOMAIN,T.VALUENAME N_DOM,PRIME_ID,M.DATE_BORN ,NPLV,GOD_NPLV,NPRT,GOD_NPRT,NRUL,TH` |
| 10 | 6ustff0snm9y7 | 6973010 | 82 | 9867223 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 259 | PROG=0:0 | SCHEMA=ROGACHEV_AV | `/*SDS_ID = 259, CLASS_ID = 65165, TAG_ID = 27980, LOG_USER = NDV*/ /*begin   ccy.p_journal_proizv_re` |

### Топ SQL по числу версий (VERSION_COUNT)
| # | SQL_ID | VERSION_COUNT | Executions | CPU (ms) | Источник | SQL (обрезан) |
|---|--------|------:|-----------:|---------:|----------|----------------|
| 1 | 3xac0d6rf1u5n | 801 | 0 | 28606 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 2 | 9q8kbj396t8rm | 721 | 0 | 23320 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 3 | gcvgj59vhuh3b | 710 | 0 | 13908 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 4 | a39tyx6td6whw | 664 | 0 | 18206 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 5 | d4dc5xqqnu7s6 | 654 | 0 | 22803 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 6 | 6hnyvvwrmrhx6 | 546 | 0 | 14809 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 7 | 98wu2qq5ru3p4 | 341 | 33 | 1961348 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=DEMIDOVA_EO | `/*    П О Л И М Е Р Ы    */ with base as (   select r.rul_id,          r.prt,          r.god_prt   f` |
| 8 | 4ccs552n1c18w | 309 | 0 | 34328 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 9 | 9rt5s3u69wwfa | 240 | 116 | 644907 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=142172:44 | SCHEMA=CCY | `SELECT MUNIT_ID,E_DOMAIN,T.VALUENAME N_DOM,PRIME_ID,M.DATE_BORN ,NPLV,GOD_NPLV,NPRT,GOD_NPRT,NRUL,TH` |
| 10 | f711myt0q6cma | 239 | 137558 | 13067241 | MODULE=JDBC Thin Client | ACTION=None | PROG=0:0 | SCHEMA=SYS | `insert into sys.aud$( sessionid,entryid,statement,ntimestamp#, userid,userhost,terminal,action#,retu` |

### Нагрузка по источнику (MODULE)
| MODULE | SQL | Executions | CPU (ms) | Elapsed (ms) | Buffer | Disk | RowsProc |
|--------|----:|-----------:|---------:|-------------:|-------:|-----:|--------:|
| None | 5136 | 3189482784 | 4312990856 | 10206485224 | 92968448264 | 16747130351 | 5342257479 |
| oracle@nl-sp-mes-db3 (TNS V1-V3) | 201 | 314272762 | 600368883 | 670237562 | 16874169595 | 27765471 | 486113954 |
| frmweb@nl-sp-ccm5app1 (TNS V1-V3) | 3404 | 3977817710 | 186895933 | 278877393 | 45721108340 | 349125127 | 3592270178 |
| JDBC Thin Client | 1152 | 1741068540 | 72257147 | 203050813 | 19873873640 | 1377125602 | 1116405346 |
| /nlmk.l3.ccm.pds/get_roll_type | 15 | 6174321223 | 46878513 | 51308045 | 3791327605 | 418591 | 6509904625 |
| oracle@nl-sp-ccm4-db1 (TNS V1-V3) | 9 | 85718346 | 38393375 | 53948401 | 232057286 | 644524 | 85668652 |
| DBMS_SCHEDULER | 1053 | 1975542 | 34106252 | 106681329 | 4843822740 | 138853509 | 266642760 |
| NLMKReport.exe | 546 | 104624502 | 29087323 | 106437284 | 6093460353 | 72098116 | 53317682 |
| java@nl-sp-ccm5app1 (TNS V1-V3) | 926 | 2001470994 | 23885815 | 98559562 | 16096619116 | 140560641 | 1564789389 |
| /nlmk.l3.ccm.pds/create_label | 24 | 108526 | 8419905 | 89759602 | 2395226258 | 2169702880 | 105520 |
| PL/SQL Developer | 58 | 96897 | 6368469 | 18496561 | 965581394 | 17904702 | 150801 |
| emagent_SQL_oracle_database | 85 | 1130298 | 6216787 | 15469568 | 48365450 | 11764285 | 3465398 |
| /nlmk.l3.ccm.pds/create_forming | 59 | 1563117 | 5078628 | 11391756 | 1030507350 | 67099191 | 1516417 |
| Streams | 8 | 6044045 | 3988118 | 4372018 | 4201019 | 119499 | 4066533 |
| M1400_PROC | 44 | 283847 | 2159805 | 10805320 | 524413090 | 909401 | 230551 |
| zabbix_proxy: odbc poller #12 [got 0 values in 0 | 1 | 168691 | 1782054 | 23797180 | 32051292 | 989 | 6072876 |
| oracle@nl-sp-ccm2-db2 (TNS V1-V3) | 41 | 41987631 | 1298337 | 26216698 | 428342695 | 13098855 | 36434452 |
| oracle@nl-sp-rail-db1 (TNS V1-V3) | 56 | 11876746 | 1245931 | 2331955 | 581435638 | 355792 | 9553163 |
| APP3_PROC | 94 | 33015646 | 1245091 | 8928896 | 384435089 | 6330928 | 35090381 |
| frmweb@nl-sp-f10app2-1 (TNS V1-V3) | 39 | 938037 | 888909 | 3538006 | 175676082 | 2206068 | 1856982 |

### Нагрузка по схеме парсинга (PARSING_SCHEMA_NAME)
| SCHEMA | SQL | Executions | CPU (ms) | Elapsed (ms) | Buffer | Disk |
|--------|----:|-----------:|---------:|-------------:|-------:|-----:|
| TM | 167 | 44249137 | 2314077356 | 4870370225 | 44450311255 | 6262329708 |
| L2COMMON | 420 | 298547870 | 1778564876 | 4662799164 | 29249318241 | 5849838193 |
| CCY | 2971 | 8117872566 | 394814752 | 1016278972 | 78148302820 | 6924726607 |
| SUPLPC5 | 68 | 2877124 | 335476067 | 355883907 | 6225162976 | 9181166 |
| CP | 540 | 6238306417 | 121726355 | 133152128 | 4153785073 | 2572044 |
| SYS | 2325 | 1203488951 | 98478043 | 191504234 | 8979090892 | 125042762 |
| IGSTSPEP | 39 | 1145729 | 97323207 | 111002120 | 3479991663 | 5048980 |
| HLYSTOVA_LV2 | 15 | 1395809 | 88002679 | 91366100 | 4452370326 | 3590994 |
| PETROV_VA | 17 | 64592 | 24450237 | 88929333 | 3630377605 | 56399586 |
| DUSHKIN_SN | 3 | 30726 | 16743405 | 18436202 | 1755288704 | 562967 |
| VOSTRIKOV_AS | 47 | 332858064 | 12686381 | 22057595 | 3033658330 | 2116331 |
| ARPL | 49 | 9787115 | 12339446 | 44457101 | 651250641 | 658551 |
| ARM_SHIPMENT | 169 | 285853 | 10920375 | 46900700 | 4956814818 | 1345344250 |
| SAMARINA_YV | 15 | 69222 | 9612102 | 12929658 | 2977900226 | 579717 |
| MARKOV_AN | 5 | 457022 | 9572947 | 17650452 | 3878719427 | 9254571 |

### Нагрузка по типу команды (COMMAND_TYPE)
| COMMAND_TYPE | SQL | Executions | CPU (ms) |
|--------------|----:|-----------:|---------:|
| 47 | 559 | 38829437 | 4129324162 |
| 3 | 10865 | 17186385464 | 1191069269 |
| 189 | 34 | 2082260 | 36027807 |
| 2 | 992 | 480317717 | 18499065 |
| 7 | 476 | 55262200 | 6606319 |
| 6 | 3198 | 66916885 | 5600331 |
| 170 | 6 | 40895 | 2087740 |
| 90 | 1 | 2222616 | 48147 |
| 1 | 32 | 32 | 87 |
| 26 | 37 | 76 | 9 |

### Агрегаты по пулу
- Всего SQL: **16200**
- Суммарно исполнений: **17832057582**
- CPU: **5389262941 ms**
- Время выполнения: **12009343723 ms**
- Буферные чтения: **214652082988**
- Дисковые чтения: **21150950692**
- Сортировки: **1203931752**
- Парсинги (PARSE_CALLS): **823514249**
- Обработанные строки (ROWS_PROCESSED): **19313633049**
- Занимаемая память пула (SHARABLE_MEM): **1.6 GB**

### SQL с высоким VERSION_COUNT (>20) — кандидаты на проблему версионирования
| SQL_ID | VersionCount | Источник | SQL |
|--------|-------------:|----------|-----|
| 4ccs552n1c18w | 309 | MODULE=None | PROG=0:0 | SCHEMA=L2COMMON | `SELECT /*+ OPT_PARAM('_fix_control' '9088510:0') NO_XML_QUERY_REWRITE cursor_sharing_exact */ count(` |
| 81aj4367ac18z | 34 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `UPDATE sys.snap$ SET status = :1, rscn = :2, refhnt = :3,         flag = :4, flag3 = :5  WHERE vname` |
| 28vmfpk12n23w | 32 | MODULE=None | PROG=4111:2232 | SCHEMA=SYS | `SELECT SUBSTR(MLINK,2), TNAME, USLOG, SUBSTR(AUTO_FAST,1,1), BITAND(FLAG, :B4 ), BITAND(FLAG, :B3 ) ` |
| d6p7rdc4503zz | 27 | MODULE=JDBC Thin Client | ACTION=None | PROG=142535:875 | SCHEMA=CCY | `UPDATE RUL SET H_NOM = :B3 , B_NOM = :B2 WHERE RUL_ID = :B1 ` |
| fsbqktj5vw6n9 | 27 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select next_run_date, obj#, run_job, sch_job from (select decode(bitand(a.flags, 16384), 0, a.next_r` |
| fn4tbqkw107sr | 173 | MODULE=None | PROG=0:0 | SCHEMA=PLAN_BASE | `select t.outputmaterialid, t.seqnr, t.stockingpointid, p.vid_product, p.standardproductid, p.orderco` |
| at7b12t69sk71 | 36 | MODULE=None | PROG=1989861:2133 | SCHEMA=CCY | `SELECT '+' ACT, T.RUL_ID, '15/' || TO_CHAR(T.RUL_ID) MES_ID, TO_CHAR(T.RUL_ID) || '~' || TO_CHAR(T.I` |
| 2syvqzbxp4k9z | 60 | MODULE=None | PROG=0:0 | SCHEMA=SYS | `select u.name, o.name, a.interface_version#, o.obj#      from association$ a, user$ u, obj$ o       ` |
| fw07hvxqr8ms5 | 27 | MODULE=JDBC Thin Client | ACTION=None | PROG=142535:763 | SCHEMA=CCY | `UPDATE RUL SET IND_POVT_OBR = 0, NPRT = :B31 , INDD = NULL, GODPR = :B30 , ID_NAZN = NVL(:B29 ,:B28 ` |
| 14d1mhxdacpj1 | 21 | MODULE=JDBC Thin Client | ACTION=None | PROG=151561:32 | SCHEMA=CCY | `SELECT R.RUL_ID ,W.OPER_CODE ,W.AGR_SAP, W.N_AGR_SAP ,W.N_OPER_NAME FROM RUL R, RUL P, MV_OPER_SAP W` |

### SQL с инвалидациями (INVALIDATIONS>0)
| SQL_ID | Invalidations | Источник | SQL |
|--------|--------------:|----------|-----|
| 1kb95r9u5c00q | 1 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=2281679:26 | SCHEMA=CCY | `SELECT COUNT(DISTINCT R.RUL_ID) FROM RUL R WHERE R.AKTUAL<>-1 AND R.IND_RAZMAT IS NULL AND (R.IND_ZA` |
| 87xua46kus01y | 5 | MODULE=None | PROG=0:0 | SCHEMA=TPAR | `insert into data16 (meter_time, agr_id, v, nataj_nach, udlin_prav, nataj_prav, glubina_1, glubina_2)` |
| a9fjnuzzsc052 | 1 | MODULE=None | PROG=0:0 | SCHEMA=LEV2 | `insert into operation (agr_id, oper_id, oper_start, mode_code) values (:1, :2, :3, :4)` |
| 3xpcrwq5ws05h | 2 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=145492:14 | SCHEMA=CCY | `SELECT R.TIP_POKR, S.KOD_SOST_RUL FROM RUL R, SOST_RUL S WHERE R.RUL_ID = :B1 AND S.RUL_ID = R.RUL_I` |
| 6w5qthupyc09d | 1 | MODULE=JDBC Thin Client | ACTION=None | PROG=142535:2682 | SCHEMA=CCY | `SELECT R.RUL_ID_PARENT,R.IND_RAZMAT,R.AKTUAL,P.IND_POVT,P.IND_ZAPRAV,P.IND_RAZMAT RAZM_P ,DECODE(A.T` |
| 59h8f8t6cs09j | 2 | MODULE=java@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=0:0 | SCHEMA=DOBRYNINA_MV | `SELECT COUNT (*) FROM SOLID_DEF S   WHERE S.RUL_ID IN (SELECT RUL_ID FROM STRAKT T   WHERE T.IDAKT =` |
| cat9kz4nkn0a3 | 1 | MODULE=None | PROG=2022010:5 | SCHEMA=SYS | `INSERT INTO ADM.AUD_HIST SELECT * FROM SYS.AUD$ WHERE NTIMESTAMP# < :B1 ` |
| 9fwrf1z56h0ar | 2 | MODULE=ORACLE.EXE | ACTION=None | PROG=2217753:16 | SCHEMA=L2COMMON | `select offset, r2, length from vcom_profile_points where prime_id = :1` |
| gcvtzwy4tc0bb | 1 | MODULE=frmweb@nl-sp-ccm5app1 (TNS V1-V3) | ACTION=None | PROG=65131:927 | SCHEMA=CCY | `SELECT SUM(R.VES)/1000 FROM RUL R WHERE R.UPAK = :B2 AND R.ID_POS_ZAK = :B1 ` |
| 93acc3g7480dy | 3 | MODULE=NLMKReport.exe | ACTION=SDS_ID = 223 | PROG=0:0 | SCHEMA=PETROV_VA | `select sum(VES ) as VES_1,sum(VESNOBR ) as VESNOBR_2,sum(VESOBR ) as VESOBR_3 from (/*SDS_ID = 223, ` |

---

## ccm5 / table_sizes

_Файл_: `./spider-output/ccm5_table_sizes.json`, строк: 142

_Колонки_: SEGMENT_NAME (VARCHAR2), SEGMENT_TYPE (VARCHAR2), BYTES (NUMBER)

**Сегменты**: 142 шт, суммарно **14.2 GB**

### Топ сегментов по размеру
| # | SEGMENT_NAME | SEGMENT_TYPE | BYTES |
|---|--------------|--------------|------:|
| 1 | MEASUREMENTS2_PK | INDEX | 12213813248 (11.4 GB) |
| 2 | OUTCARTS_LOG | TABLE PARTITION | 645464064 (615.6 MB) |
| 3 | OUTCARTS | TABLE PARTITION | 272039936 (259.4 MB) |
| 4 | INCARTS_LOG | TABLE PARTITION | 220921856 (210.7 MB) |
| 5 | OUTCARTS_LOG_OUTCART_CID_FK | INDEX | 176160768 (168.0 MB) |
| 6 | INCARTS | TABLE PARTITION | 151846912 (144.8 MB) |
| 7 | OUTCARTS_LOG_PK | INDEX | 125829120 (120.0 MB) |
| 8 | FRAGMENT_INTERVALS_PK | INDEX | 117440512 (112.0 MB) |
| 9 | FRAGMENT_INTERVALS | TABLE PARTITION | 100663296 (96.0 MB) |
| 10 | INTERVALS_PK | INDEX | 92274688 (88.0 MB) |
| 11 | OUTCARTS_AK | INDEX | 83886080 (80.0 MB) |
| 12 | INTERVALS | TABLE PARTITION | 67108864 (64.0 MB) |
| 13 | FRAGMENT_INTERVALS_SECT_ID_FK | INDEX | 66060288 (63.0 MB) |
| 14 | OUTCARTS_PRIME_ID_I | INDEX | 63569920 (60.6 MB) |
| 15 | FRAGMENT_INTERVALS_STAGE_ID_FK | INDEX | 60817408 (58.0 MB) |
| 16 | INTERVALS_STRIPCID_SECT_IDX | INDEX | 59768832 (57.0 MB) |
| 17 | INCARTS_LOG_PK | INDEX | 56623104 (54.0 MB) |
| 18 | INCARTS_AK | INDEX | 50790400 (48.4 MB) |
| 19 | OUTCARTS_PROCTYPE_ID_FK | INDEX | 48758784 (46.5 MB) |
| 20 | INTERVALS_SECT_ID_FK | INDEX | 48234496 (46.0 MB) |

### Размер по типу сегмента
| SEGMENT_TYPE | Суммарно |
|--------------|---------:|
| INDEX | 12.7 GB |
| TABLE PARTITION | 1.4 GB |
| TABLE | 93.9 MB |
| LOBINDEX | 960.0 KB |
| LOBSEGMENT | 960.0 KB |

---

## ccm5 / tablespaces

_Файл_: `./spider-output/ccm5_tablespaces.json`, строк: 36

_Колонки_: TABLESPACE_NAME (VARCHAR2), BYTES (NUMBER)

**Табличные пространства**: 36 шт

| NAME | TYPE | SIZE | FREE | USED % |
|------|------|-----:|-----:|-------:|
| TPARTABL |  | 189.5 GB | 189.5 GB | 0.0% |
| CCYINDX |  | 160.1 GB | 160.1 GB | 0.0% |
| CCYTABL |  | 145.7 GB | 145.7 GB | 0.0% |
| CPTABL |  | 114.3 GB | 114.3 GB | 0.0% |
| PITABL |  | 78.6 GB | 78.6 GB | 0.0% |
| USERS |  | 40.7 GB | 40.7 GB | 0.0% |
| UNDOTBS1 |  | 31.1 GB | 31.1 GB | 0.0% |
| L2MSR |  | 24.7 GB | 24.7 GB | 0.0% |
| L2ANGC4I |  | 12.1 GB | 12.1 GB | 0.0% |
| L2APP3TABL |  | 9.1 GB | 9.1 GB | 0.0% |
| QLOGTABL |  | 9.0 GB | 9.0 GB | 0.0% |
| LEV2TABL |  | 8.4 GB | 8.4 GB | 0.0% |
| ETLTABL |  | 4.9 GB | 4.9 GB | 0.0% |
| SYSAUX |  | 3.8 GB | 3.8 GB | 0.0% |
| L2ANGC4T |  | 3.8 GB | 3.8 GB | 0.0% |
| L2TABL |  | 3.7 GB | 3.7 GB | 0.0% |
| PLANTABL |  | 2.0 GB | 2.0 GB | 0.0% |
| PLANINDX |  | 2.0 GB | 2.0 GB | 0.0% |
| TMTABL |  | 1.6 GB | 1.6 GB | 0.0% |
| CPINDX |  | 1.4 GB | 1.4 GB | 0.0% |
| SYSTEM |  | 1.3 GB | 1.3 GB | 0.0% |
| LEV2INDX |  | 1.3 GB | 1.3 GB | 0.0% |
| ARPLTABL |  | 1000.0 MB | 1000.0 MB | 0.0% |
| MESQUE |  | 740.0 MB | 740.0 MB | 0.0% |
| DB2KAFKATABL |  | 700.0 MB | 700.0 MB | 0.0% |
| L2RM1TABL |  | 700.0 MB | 700.0 MB | 0.0% |
| ARPLINDX |  | 500.0 MB | 500.0 MB | 0.0% |
| TOOLS |  | 200.0 MB | 200.0 MB | 0.0% |
| ODM |  | 200.0 MB | 200.0 MB | 0.0% |
| XDB |  | 200.0 MB | 200.0 MB | 0.0% |
| DRSYS |  | 200.0 MB | 200.0 MB | 0.0% |
| L2QUE |  | 200.0 MB | 200.0 MB | 0.0% |
| INDX |  | 200.0 MB | 200.0 MB | 0.0% |
| L2KAFKATABL |  | 200.0 MB | 200.0 MB | 0.0% |
| CWMLITE |  | 200.0 MB | 200.0 MB | 0.0% |
| TEMP |  | 0.0 B | 0.0 B | 0.0% |

