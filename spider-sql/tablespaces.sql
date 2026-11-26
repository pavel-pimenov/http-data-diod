SELECT ts.name AS tablespace_name, COALESCE(SUM(df.bytes),0) AS bytes FROM v$tablespace ts LEFT JOIN v$datafile df ON df.ts# = ts.ts# GROUP BY ts.name ORDER BY bytes DESC
