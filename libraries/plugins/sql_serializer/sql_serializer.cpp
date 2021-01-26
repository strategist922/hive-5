#include <hive/plugins/sql_serializer/sql_serializer_plugin.hpp>

#include <hive/chain/util/impacted.hpp>
#include <hive/protocol/config.hpp>
#include <hive/utilities/plugin_utilities.hpp>

#include <fc/io/json.hpp>
#include <fc/smart_ref_impl.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include <condition_variable>
#include <shared_mutex>
#include <thread>
#include <future>

#include "../../vendor/rocksdb/util/threadpool_imp.h"

// C++ connector library for PostgreSQL (http://pqxx.org/development/libpqxx/)
#include <pqxx/pqxx>

namespace hive
{
	namespace plugins
	{
		namespace sql_serializer
		{
			using thread_pool_t = rocksdb::ThreadPoolImpl;
			using num_t = std::atomic<uint64_t>;
			using duration_t = fc::microseconds;
			using stat_time_t = std::atomic<duration_t>;

			inline void operator+=( stat_time_t& atom, const duration_t& dur )
			{
				atom.store( atom.load() + dur );
			}

			struct stat_t
			{
				stat_time_t processing_time{ duration_t{0} };
				num_t count{0};
			};

			struct ext_stat_t : public stat_t
			{
				stat_time_t flush_time{ duration_t{0} };

				void reset()
				{
					processing_time.store( duration_t{0} );
					flush_time.store( duration_t{0} );
					count.store(0);
				}
			};

			struct stats_group
			{
				stat_time_t sending_cache_time{ duration_t{0} };
				uint64_t workers_count{0};
				uint64_t all_created_workers{0};

				ext_stat_t blocks{};
				ext_stat_t transactions{};
				ext_stat_t operations{};
				ext_stat_t virtual_operations{};

				void reset()
				{
					blocks.reset();
					transactions.reset();
					operations.reset();
					virtual_operations.reset();

					sending_cache_time.store(duration_t{0});
					workers_count = 0;
					all_created_workers = 0;
				}
			};

			inline std::ostream& operator<<(std::ostream& os, const stat_t& obj)
			{
				return os << obj.processing_time.load().count() << " us | count: " << obj.count.load();
			}

			inline std::ostream& operator<<(std::ostream& os, const ext_stat_t& obj)
			{
				return os << "flush time: " << obj.flush_time.load().count() << " us | processing time: " << obj.processing_time.load().count() << " us | count: " << obj.count.load();
			}

			inline std::ostream& operator<<(std::ostream& os, const stats_group& obj)
			{
				#define __shortcut( name ) << #name ": " << obj.name << std::endl
				return os
					<< "threads created since last info: " << obj.all_created_workers << std::endl
					<< "currently working threads: " << obj.workers_count << std::endl
					<< "sending accounts and permlinks took: " << obj.sending_cache_time.load().count() << " us"<< std::endl
					__shortcut(blocks)
					__shortcut(transactions)
					__shortcut(operations)
					__shortcut(virtual_operations)
					;
			}

			using namespace hive::protocol;
			using namespace hive::plugins::sql_serializer::PSQL;

			using work = pqxx::work;
			using chain::database;
			using chain::operation_object;

			constexpr size_t default_reservation_size{ 16'000u };
			constexpr size_t max_tuples_count{ 1'000 };
			constexpr size_t max_data_length{ 16*1024*1024 }; 

			inline void mylog(const char* msg)
			{
				std::cout << "[ " << std::this_thread::get_id() << " ] " << msg << std::endl;
			}

			namespace detail
			{
				struct transaction_repr_t
				{
					std::unique_ptr<pqxx::connection> _connection;
					std::unique_ptr<work> _transaction;

					transaction_repr_t() = default;
					transaction_repr_t(pqxx::connection* _conn, work* _trx) : _connection{_conn}, _transaction{_trx} {}
				};
				using transaction_t = std::unique_ptr<transaction_repr_t>;

				struct postgress_connection_holder
				{
					explicit postgress_connection_holder(const fc::string &url)
							: connection_string{url} {}

					transaction_t start_transaction() const
					{
						// mylog("started transaction");
						pqxx::connection* _conn = new pqxx::connection{ this->connection_string };
						work *_trx = new work{*_conn};
						_trx->exec("SET CONSTRAINTS ALL DEFERRED;");

						return transaction_t{ new transaction_repr_t{ _conn, _trx } };
					}

					bool exec_transaction(transaction_t &trx, const fc::string &sql) const
					{
						if (sql == fc::string())
							return true;
						return sql_safe_execution([&trx, &sql]() { trx->_transaction->exec(sql); }, sql.c_str());
					}

					bool exec_transaction(transaction_t &trx, const fc::string &sql, pqxx::result& result) const
					{
						if (sql == fc::string())
							return true;
						return sql_safe_execution([&trx, &sql, &result]() { result = trx->_transaction->exec(sql); }, sql.c_str());
					}

					bool commit_transaction(transaction_t &trx) const
					{
						// mylog("commiting");
						return sql_safe_execution([&]() { trx->_transaction->commit(); }, "commit");
					}

					void abort_transaction(transaction_t &trx) const
					{
						// mylog("aborting");
						trx->_transaction->abort();
					}

					bool exec_single_in_transaction(const fc::string &sql, pqxx::result *result = nullptr) const
					{
						if (sql == fc::string())
							return true;

						return sql_safe_execution([&]() {
							pqxx::connection conn{ this->connection_string };
							pqxx::work trx{conn};
							if (result)
								*result = trx.exec(sql);
							else
								trx.exec(sql);
							trx.commit();
						}, sql.c_str());
					}

					template<typename T>
					bool get_single_value(const fc::string& query, T& _return) const
					{
						pqxx::result res;
						if(!exec_single_in_transaction( query, &res ) && res.empty() ) return false;
						_return = res.at(0).at(0).as<T>();
						return true;
					}

					template<typename T>
					T get_single_value(const fc::string& query) const
					{
						T _return;
						FC_ASSERT( get_single_value( query, _return ) );
						return _return;
					}

					uint get_max_transaction_count() const
					{
						// get maximum amount of connections defined by postgres and set half of it as max; minimum is 1
						return std::max( 1u, get_single_value<uint>("SELECT setting::int / 2 FROM pg_settings WHERE  name = 'max_connections'") );
					}

				private:
					fc::string connection_string;

					bool sql_safe_execution(const std::function<void()> &f, const char* msg = nullptr) const
					{
						try
						{
							f();
							return true;
						}
						catch (const pqxx::pqxx_exception &sql_e)
						{
							elog("Exception from postgress: ${e}", ("e", sql_e.base().what()));
						}
						catch (const std::exception &e)
						{
							elog("Exception: ${e}", ("e", e.what()));
						}
						catch (...)
						{
							elog("Unknown exception occured");
						}
						if(msg) std::cerr << "Error message: " << msg << std::endl;
						return false;
					}
				};

				struct cached_data_t
				{
					std::vector<PSQL::processing_objects::process_block_t> blocks;
					std::vector<PSQL::processing_objects::process_transaction_t> transactions;
					std::vector<PSQL::processing_objects::process_operation_t> operations;
					std::vector<PSQL::processing_objects::process_virtual_operation_t> virtual_operations;

					size_t total_size{ 0ul };

					cached_data_t(const size_t reservation_size)
					{
						blocks.reserve(reservation_size);
						transactions.reserve(reservation_size);
						operations.reserve(reservation_size);
						virtual_operations.reserve(reservation_size);
					}

					~cached_data_t()
					{
						log_size("destructor");
					}

					void log_size(const fc::string& msg = "size")
					{
						return;
						mylog(generate([&](fc::string& ss){ 
							ss = msg + ": ";
							ss += "blocks: " + std::to_string(blocks.size());
							ss += " trx: " + std::to_string(transactions.size());
							ss += " operations: " + std::to_string(operations.size());
							ss += " vops: " + std::to_string(virtual_operations.size());
							ss += " total size: " + std::to_string(total_size);
							ss += "\n"; 
						}).c_str());
					}
				};
				using cached_containter_t = std::unique_ptr<cached_data_t>;

				struct flush_task
				{
					postgress_connection_holder& conn;
					ext_stat_t& stat;
					std::shared_timed_mutex mtx{};
					std::condition_variable_any cv{};
					std::atomic_bool is_table_used{false};

					void operator()(std::shared_ptr<fc::string> sql)
					{
						if(sql.get() == nullptr) return;

						std::shared_lock<std::shared_timed_mutex> lck{ mtx };
						while(is_table_used.load()) cv.wait(lck);
						is_table_used.store(true);

						{
							const auto before = fc::time_point::now();
							conn.exec_single_in_transaction(std::move(*sql));
							stat.flush_time += fc::time_point::now() - before;
						}

						is_table_used.store(false);
						cv.notify_one();
					}
				};

				struct task_collection_t
				{
					flush_task block_task;
					flush_task trx_task;
					flush_task op_task;
					flush_task vop_task;

					task_collection_t( postgress_connection_holder& conn, stats_group& stats ) : 
						block_task{conn, stats.blocks}, 
						trx_task{conn, stats.transactions}, 
						op_task{conn, stats.operations}, 
						vop_task{conn, stats.virtual_operations} 
					{}
				};

				struct process_task
				{
					// just to make it possible to store in ThreadPool
					std::shared_ptr<cached_containter_t> input;
					postgress_connection_holder& conn;
					task_collection_t& tasks;
					stats_group& stats;

					void upload_caches(PSQL::sql_dumper &dumper)
					{
						fc::string perms;
						dumper.get_dumped_cache(perms);
						if ( perms != fc::string()) conn.exec_single_in_transaction(std::move(perms));
					}

					void operator()()
					{
						PSQL::sql_dumper dumper{};
						cached_data_t& data = **input;	// alias
						auto tm = fc::time_point::now();

						std::vector< std::future<void> > futures;

						auto update_stat = [&](ext_stat_t& s, const uint64_t cnt)
						{
							s.processing_time += fc::time_point::now() - tm;
							s.count += cnt;
							tm = fc::time_point::now();
						};
						auto update_flushing_stat = [&](stat_time_t& s)
						{
							s += fc::time_point::now() - tm;
							tm = fc::time_point::now();
						};
						auto add = [&](fc::string&& _sql, flush_task& _task)
						{
							futures.emplace_back( std::async(std::launch::async, [&](fc::string&& sql, flush_task& task){ task( std::make_shared<fc::string>( std::move(sql) ) ); }, std::move(_sql), std::ref(_task) ) );
						};

						for(const auto& op : data.operations) dumper.process_operation(op);
						update_stat(stats.operations, data.operations.size());
						data.operations.clear();

						for(const auto& vop : data.virtual_operations) dumper.process_virtual_operation(vop);
						update_stat(stats.virtual_operations, data.virtual_operations.size());
						data.virtual_operations.clear();

						upload_caches(dumper);
						update_flushing_stat(stats.sending_cache_time);

						add( dumper.get_operations_sql(), tasks.op_task );
						add( dumper.get_virtual_operations_sql(), tasks.vop_task );

						for(const auto& block : data.blocks) dumper.process_block(block);
						update_stat(stats.blocks, data.blocks.size());
						add( dumper.get_blocks_sql(), tasks.block_task );
						data.blocks.clear();

						for(const auto& trx : data.transactions) dumper.process_transaction(trx);
						update_stat(stats.transactions, data.transactions.size());
						add( dumper.get_transaction_sql(), tasks.trx_task );
						data.transactions.clear();

						for(auto& f : futures) f.wait();
					}
				};

				constexpr int table_count = sizeof(task_collection_t) / sizeof(flush_task);	// blocks, trx, op, vops
				constexpr uint connections_per_worker{ 2u };	// accounts, permlinks

				class sql_serializer_plugin_impl
				{
				public:
					sql_serializer_plugin_impl(const std::string &url) 
						: connection{url}, 
							tasks{connection, current_stats}
					{
						worker_pool.SetBackgroundThreads( std::max( 1u, connection.get_max_transaction_count() / connections_per_worker ) );

						pqxx::result ret;
						auto trx = connection.start_transaction();
						if(connection.exec_transaction(trx, "SELECT name, id, current_counter FROM hive_accounts", ret))
						{
							for(const auto& row : ret)
									accounts[row.at(0).as<fc::string>()] = id_counter_t(row.at(1).as<uint32_t>(), row.at(2).as<uint32_t>());
						}else elog("Failed to get accounts");
						connection.exec_transaction(trx, "DELETE FROM hive_accounts");
						connection.commit_transaction(trx);
					}

					virtual ~sql_serializer_plugin_impl() 
					{
						connection.exec_single_in_transaction(get_account_insert_query());
					}

					boost::signals2::connection _on_post_apply_operation_con;
					boost::signals2::connection _on_post_apply_transaction_con;
					boost::signals2::connection _on_post_apply_block_con;
					boost::signals2::connection _on_starting_reindex;
					boost::signals2::connection _on_finished_reindex;
					boost::signals2::connection _on_live_sync_start;

					postgress_connection_holder connection;
					fc::optional<fc::string> path_to_schema;
					bool set_index = false;
					bool are_constraints_active = true;

					using thread_with_status = std::pair<std::shared_ptr<std::thread>, bool>;
					std::list<thread_with_status> threads;
					uint32_t blocks_per_commit = 1;
					int64_t block_vops = 0;
					uint64_t op_counter = 0;

					accountname_id_counter_map_t accounts{};
					cached_containter_t currently_caching_data;
					thread_pool_t worker_pool{};
					task_collection_t tasks;
					stats_group current_stats;

					id_counter_t get_id_with_counter(const account_name_type& name)
					{
						auto it_b = accounts.insert( typename accountname_id_counter_map_t::value_type{ name, id_counter_t{ accounts.size(), 0 } } );

						if(!it_b.second) it_b.first->second.second += 1;

						return it_b.first->second;
					}

					std::vector<id_counter_t> get_participants_ids(const hive::protocol::operation& op)
					{
						PSQL::account_names_container_t impacted;
						hive::app::operation_get_impacted_accounts(op, impacted);
						impacted.erase(account_name_type{});
						std::vector<id_counter_t> ret; 
						ret.reserve(impacted.size());
						for(const account_name_type& acc : impacted) ret.emplace_back( get_id_with_counter( acc ) );
						return std::move( ret );
					}

					fc::string get_account_insert_query()
					{
						fc::string query{ "INSERT INTO hive_accounts(name, id,  current_counter) VALUES " };

						for(auto it = accounts.begin(); it != accounts.end(); it++)
						{
							if(it != accounts.begin()) query += ",";
							const auto& pair = it->second;
							query += fc::string{"( "} + escapings::escape_sql( it->first ) + ", " + std::to_string(pair.first) + ", " + std::to_string(pair.second) + " )";
						}

						return std::move(query);
					}

					void log_statistics()
					{
						current_stats.workers_count = worker_pool.GetQueueLen();
						std::cout << current_stats;
						current_stats.reset();
					}

					void switch_indexes(const bool active) const
					{
						using up_and_down_index = std::pair<const char*, const char*>;
						static const std::vector<up_and_down_index> indexes{{up_and_down_index
							{R"(CREATE INDEX IF NOT EXISTS hive_operations_operation_types_index ON "hive_operations" ("op_type_id"))", "DROP INDEX IF EXISTS hive_operations_operation_types_index"},
							{R"(CREATE INDEX IF NOT EXISTS hive_operations_participants_index ON "hive_operations" USING GIN ("participants" gin__int_ops))", "DROP INDEX IF EXISTS hive_operations_participants_index"},
							{R"(CREATE INDEX IF NOT EXISTS hive_operations_permlink_ids_index ON "hive_operations" USING GIN ("permlink_ids" gin__int_ops))", "DROP INDEX IF EXISTS hive_operations_permlink_ids_index"},
							{R"(CREATE INDEX IF NOT EXISTS hive_virtual_operations_operation_types_index ON "hive_virtual_operations" ("op_type_id"))", "DROP INDEX IF EXISTS hive_virtual_operations_operation_types_index"},
							{R"(CREATE INDEX IF NOT EXISTS hive_virtual_operations_participants_index ON "hive_virtual_operations" USING GIN ("participants" gin__int_ops))", "DROP INDEX IF EXISTS hive_virtual_operations_participants_index"},
							{R"(CREATE INDEX IF NOT EXISTS hive_virtual_operations_block_num_index ON "hive_virtual_operations"( "block_num" ))", "DROP INDEX IF EXISTS hive_virtual_operations_block_num_index"}
							// {R"(CREATE INDEX IF NOT EXISTS hive_virtual_operations_order_id_index ON "hive_virtual_operations"( "order_id" ))", "DROP INDEX IF EXISTS hive_virtual_operations_order_id_index"},
							// {R"(CREATE INDEX IF NOT EXISTS hive_operations_order_id_index ON "hive_operations"( "order_id" ))", "DROP INDEX IF EXISTS hive_operations_order_id_index"}
						}};

						auto trx = connection.start_transaction();
						for (const auto &idx : indexes)
						{
							const char * sql =  ( active ? idx.first : idx.second );
							if(!connection.exec_transaction(trx, sql))
							{
								connection.abort_transaction(trx);
								FC_ASSERT( false );
							}
						}
						connection.commit_transaction(trx);
					}

					void recreate_db()
					{
						FC_ASSERT(path_to_schema.valid());

						std::ifstream file{*path_to_schema};
						fc::string q{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
						file.close();

						if(!connection.exec_single_in_transaction(q)) 
							wlog("Failed to execute query from ${schema_path}:\n${query}", ("schema_path", *path_to_schema)("query", q));
					}

					void init_database()
					{
						connection.exec_single_in_transaction(PSQL::get_all_type_definitions());
					}

					void close_pools()
					{
						// finish all the workers
						worker_pool.WaitForJobsAndJoinAllThreads();
					}

					void switch_constraints(const bool active)
					{
						if(are_constraints_active == active) return;
						else are_constraints_active = active;

						using up_and_down_constraint = std::pair<const char*, const char*>;
						static const std::vector<up_and_down_constraint> constrainsts{{	up_and_down_constraint
							{"ALTER TABLE hive_transactions ADD CONSTRAINT hive_transactions_fk_1 FOREIGN KEY (block_num) REFERENCES hive_blocks (num)","ALTER TABLE hive_transactions DROP CONSTRAINT IF EXISTS hive_transactions_fk_1"},
							{"ALTER TABLE hive_operations ADD CONSTRAINT hive_operations_fk_1 FOREIGN KEY (op_type_id) REFERENCES hive_operation_types (id)","ALTER TABLE hive_operations DROP CONSTRAINT IF EXISTS hive_operations_fk_1"},
							{"ALTER TABLE hive_operations ADD CONSTRAINT hive_operations_fk_2 FOREIGN KEY (block_num, trx_in_block) REFERENCES hive_transactions (block_num, trx_in_block)","ALTER TABLE hive_operations DROP CONSTRAINT IF EXISTS hive_operations_fk_2"},
							{"ALTER TABLE hive_virtual_operations ADD CONSTRAINT hive_virtual_operations_fk_1 FOREIGN KEY (op_type_id) REFERENCES hive_operation_types (id)","ALTER TABLE hive_virtual_operations DROP CONSTRAINT IF EXISTS hive_virtual_operations_fk_1"},
							{"ALTER TABLE hive_virtual_operations ADD CONSTRAINT hive_virtual_operations_fk_2 FOREIGN KEY (block_num) REFERENCES hive_blocks (num)","ALTER TABLE hive_virtual_operations DROP CONSTRAINT IF EXISTS hive_virtual_operations_fk_2"}
						}};

						auto trx = connection.start_transaction();
						for (const auto &idx : constrainsts)
						{
							const char * sql =  ( active ? idx.first : idx.second );
							if(!connection.exec_transaction(trx, sql))
							{
								connection.abort_transaction(trx);
								FC_ASSERT( false );
							}
						}
						connection.commit_transaction(trx);
					}

					void process_cached_data()
					{
						if(currently_caching_data.get() == nullptr) return;
						current_stats.all_created_workers++;
						worker_pool.SubmitJob( process_task{ std::make_shared<cached_containter_t>( std::move( currently_caching_data ) ), connection, tasks, current_stats } );
						// process_task{ std::make_shared<cached_containter_t>( std::move( currently_caching_data ) ), connection, tasks, current_stats }();
					}

					void push_currently_cached_data(const size_t reserve)
					{
						process_cached_data();
						if (reserve)
							currently_caching_data = std::make_unique<cached_data_t>(reserve);
					}

					void cleanup_sequence()
					{
						ilog("Flushing rest of data, wait a moment...");
						push_currently_cached_data(0);
						close_pools();
						if(are_constraints_active) return;
						if( !appbase::app().is_interrupt_request() )
						{
							ilog("Creating indexes on user request");
							switch_indexes(true);
						}
						ilog("Enabling constraints...");
						switch_constraints(true);
						ilog("Done, cleanup complete");
					}
				};
			} // namespace detail

			sql_serializer_plugin::sql_serializer_plugin() {}
			sql_serializer_plugin::~sql_serializer_plugin() 
			{
			}

			void sql_serializer_plugin::set_program_options(options_description &cli, options_description &cfg)
			{
				cfg.add_options()("psql-url", boost::program_options::value<string>(), "postgres connection string")("psql-path-to-schema", "if set and replay starts from 0 block, executes script");
			}

			void sql_serializer_plugin::plugin_initialize(const boost::program_options::variables_map &options)
			{
				ilog("Initializing sql serializer plugin");
				FC_ASSERT(options.count("psql-url"), "`psql-url` is required argument");
				my = std::make_unique<detail::sql_serializer_plugin_impl>(options["psql-url"].as<fc::string>());

				// settings
				if (options.count("psql-path-to-schema"))
					my->path_to_schema = options["psql-path-to-schema"].as<fc::string>();
				my->currently_caching_data = std::make_unique<detail::cached_data_t>( default_reservation_size );

				// signals
				auto &db = appbase::app().get_plugin<hive::plugins::chain::chain_plugin>().db();
				my->_on_post_apply_operation_con = db.add_post_apply_operation_handler([&](const operation_notification &note) { on_post_apply_operation(note); }, *this);
				my->_on_post_apply_transaction_con = db.add_post_apply_transaction_handler([&](const transaction_notification &note) { on_post_apply_transaction(note); }, *this);
				my->_on_post_apply_block_con = db.add_post_apply_block_handler([&](const block_notification &note) { on_post_apply_block(note); }, *this);
				my->_on_finished_reindex = db.add_post_reindex_handler([&](const reindex_notification &note) { on_post_reindex(note); }, *this);
				my->_on_starting_reindex = db.add_pre_reindex_handler([&](const reindex_notification &note) { on_pre_reindex(note); }, *this);
				my->_on_live_sync_start = appbase::app().get_plugin<hive::plugins::chain::chain_plugin>().on_sync.connect(0, [&](){ on_live_sync_start(); });
			}

			void sql_serializer_plugin::plugin_startup()
			{
				ilog("sql::plugin_startup()");
			}

			void sql_serializer_plugin::plugin_shutdown()
			{
				ilog("Flushing left data...");
				my->close_pools();

				if (my->_on_post_apply_block_con.connected())
					chain::util::disconnect_signal(my->_on_post_apply_block_con);
				if (my->_on_post_apply_transaction_con.connected())
					chain::util::disconnect_signal(my->_on_post_apply_transaction_con);
				if (my->_on_post_apply_operation_con.connected())
					chain::util::disconnect_signal(my->_on_post_apply_operation_con);
				if (my->_on_starting_reindex.connected())
					chain::util::disconnect_signal(my->_on_starting_reindex);
				if (my->_on_finished_reindex.connected())
					chain::util::disconnect_signal(my->_on_finished_reindex);
				if (my->_on_live_sync_start.connected())
					chain::util::disconnect_signal(my->_on_live_sync_start);

				ilog("Done. Connection closed");
			}

			void sql_serializer_plugin::on_post_apply_operation(const operation_notification &note)
			{
				const bool is_virtual = note.op.visit(PSQL::is_virtual_visitor{});

				// deserialization
				fc::string deserialized_op{ note.op.visit(sql_serializer::escapings::escape_visitor{}) };

				detail::cached_containter_t &cdtf = my->currently_caching_data; // alias
				cdtf->total_size += deserialized_op.size() + sizeof(note);
				if (is_virtual)
				{
					cdtf->virtual_operations.emplace_back(
							note.block,
							note.trx_in_block,
							( note.trx_in_block < 0 ? my->block_vops++ : note.op_in_trx ),
							note.op,
							std::move(deserialized_op),
							my->get_participants_ids(note.op)
						);
				}
				else
				{
					cdtf->operations.emplace_back(
							note.block,
							note.trx_in_block,
							note.op_in_trx,
							note.op,
							std::move(deserialized_op),
							my->get_participants_ids(note.op)
						);
				}
			}

			void sql_serializer_plugin::on_post_apply_transaction(const transaction_notification &note) {}

			void sql_serializer_plugin::on_post_apply_block(const block_notification &note)
			{
				for (int64_t i = 0; i < static_cast<int64_t>(note.block.transactions.size()); i++)
					handle_transaction(note.block.transactions[i].id(), note.block_num, i);

				my->currently_caching_data->total_size += note.block_id.data_size() + sizeof(note.block_num);
				my->currently_caching_data->blocks.emplace_back(
						note.block_id,
						note.block_num,
						note.block.timestamp);
				my->block_vops = 0;

				if( my->currently_caching_data->total_size >= max_data_length || note.block_num % my->blocks_per_commit == 0 )
				{
					my->push_currently_cached_data(prereservation_size);
				}

				if( note.block_num % 100'000 == 0 )
				{
					// my->worker_pool.JoinAllThreads();
					my->log_statistics();
				}
			}

			void sql_serializer_plugin::handle_transaction(const hive::protocol::transaction_id_type &hash, const int64_t block_num, const int64_t trx_in_block)
			{
				my->currently_caching_data->total_size += sizeof(hash) + sizeof(block_num) + sizeof(trx_in_block);
				my->currently_caching_data->transactions.emplace_back(
						hash,
						block_num,
						trx_in_block);
			}

			void sql_serializer_plugin::on_pre_reindex(const reindex_notification &note)
			{
				my->switch_constraints(false);
        my->switch_indexes(false);
        my->set_index = true; /// Define indices once reindex done.
				if(note.force_replay && my->path_to_schema.valid()) my->recreate_db();
				my->init_database();
				my->blocks_per_commit = 10'000;
			}

			void sql_serializer_plugin::on_post_reindex(const reindex_notification &)
			{
				mylog("finishing from post reindex");
				my->cleanup_sequence();
				my->blocks_per_commit = 1;
			}
			
			void sql_serializer_plugin::on_live_sync_start() {}

		} // namespace sql_serializer
	}		// namespace plugins
} // namespace hive
