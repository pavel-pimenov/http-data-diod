

rem curl -k --request POST -H "Content-Type: application/json" --data-raw "{\"jsonrpc\": \"2.0\", \"method\": \"GetHeatData\", \"id\": 1, \"params\": {\"timestamp\": 0, \"p_heat_no\": 106}}" -v https://172.23.201.2/DataHub/DataHubSrv
rem curl -u login2:pass2 -k --request POST -H "Content-Type: application/json" --data-raw "{\"jsonrpc\": \"2.0\", \"method\": \"GetHeatData\", \"id\": 1, \"params\": {\"timestamp\": 0, \"p_heat_no\": 107}}" -v https://odskc2.nlmk/DataHub/DataHubSrv
curl -k --request POST -H "Content-Type: application/json" --data-raw "{\"jsonrpc\": \"2.0\", \"method\": \"GetHeatData\", \"id\": 1, \"params\": {\"timestamp\": 0, \"p_heat_no\": aaa}}" -v https://odskc2.nlmk/DataHub/DataHubSrv

