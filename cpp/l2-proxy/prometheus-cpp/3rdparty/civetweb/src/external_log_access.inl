/* No-op: access log is not written for metrics-only server */
static void
log_access(const struct mg_connection *conn)
{
	(void)conn;
}
