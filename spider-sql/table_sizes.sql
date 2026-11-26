SELECT segment_name, segment_type, SUM(bytes) AS bytes FROM user_segments GROUP BY segment_name, segment_type ORDER BY bytes DESC
