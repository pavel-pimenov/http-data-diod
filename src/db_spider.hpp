#ifndef DB_SPIDER_HPP
#define DB_SPIDER_HPP

#include "app_context.hpp"
#include "config.hpp"
#include "db_query_handler.hpp"
#include "nlohmann/json.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// l2-db-spider: периодически опрашивает сконфигурированные базы данных
// произвольными SELECT-запросами (из INI-конфига DB_SPIDER_CONFIG_FILE) и
// пишет результат в JSON-файл на диске — по одному файлу <db>_<query>.json
// на каждый запрос. Переиспользует DbQueryHandler (и его executor'ы) для
// выполнения запросов, поэтому поддерживает Oracle и PostgreSQL.
//
// Жизненный цикл: run() запускает цикл опроса (сразу + по таймеру
// m_interval_seconds), пока не будет запрошена остановка через stop_token.
class DbSpider {
public:
  explicit DbSpider(AppContext &context);
  ~DbSpider();

  DbSpider(const DbSpider &) = delete;
  DbSpider &operator=(const DbSpider &) = delete;

  // Запускает цикл опроса. Блокирует до запроса остановки.
  void run(const std::stop_token &st);

  // Выполняет один полный цикл опроса всех баз и возвращает собранный JSON.
  // Используется тестами для детерминированного запуска без таймера.
  [[nodiscard]] json run_poll();

private:
  // Опрашивает одну базу: все её запросы, пишет <db>_<query>.json потоково.
  json poll_database(Config::Spider::Db &db);
  // Открывает файл <output_dir>/<db>_<query>.json и потоково пишет
  // {generated_at, db, query, status, columns, row_count, rows:[...]}. Каждая
  // строка сериализуется через json.dump(), склейка в массив — потоково в
  // файле. Возвращает HTTP-статус результата (200 — успех, иначе файл
  // удаляется). Пишет маленький error-объект в файл при ошибке.
  int stream_output(Config::Spider::Db &db,
                    const Config::Spider::DbQuery &query);
  // Обновляет метрики по завершённому циклу опроса.
  void record_metrics(const json &result, const std::chrono::steady_clock::time_point &start);

  AppContext &m_ctx;
  // Хендлер, владеющий executor'ами для всех сконфигурированных баз.
  std::unique_ptr<DbQueryHandler> m_handler;
  std::vector<Config::Spider::Db> m_databases;
};

#endif // DB_SPIDER_HPP
