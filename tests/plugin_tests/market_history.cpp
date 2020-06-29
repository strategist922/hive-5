#ifdef IS_TEST_NET
#include <boost/test/unit_test.hpp>

#include <hive/chain/account_object.hpp>
#include <hive/chain/comment_object.hpp>
#include <hive/protocol/hive_operations.hpp>

#include <hive/plugins/market_history/market_history_plugin.hpp>

#include "../db_fixture/database_fixture.hpp"

using namespace hive::chain;
using namespace hive::protocol;

BOOST_FIXTURE_TEST_SUITE( market_history, database_fixture )

BOOST_AUTO_TEST_CASE( mh_test )
{
  using namespace hive::plugins::market_history;

  try
  {
    int argc = boost::unit_test::framework::master_test_suite().argc;
    char** argv = boost::unit_test::framework::master_test_suite().argv;
    for( int i=1; i<argc; i++ )
    {
      const std::string arg = argv[i];
      if( arg == "--record-assert-trip" )
        fc::enable_record_assert_trip = true;
      if( arg == "--show-test-names" )
        std::cout << "running test " << boost::unit_test::framework::current_test_case().p_name << std::endl;
    }

    appbase::app().register_plugin< market_history_plugin >();
    db_plugin = &appbase::app().register_plugin< hive::plugins::debug_node::debug_node_plugin >();
    init_account_pub_key = init_account_priv_key.get_public_key();

    db_plugin->logging = false;
    appbase::app().initialize<
      hive::plugins::market_history::market_history_plugin,
      hive::plugins::debug_node::debug_node_plugin
    >( argc, argv );

    db = &appbase::app().get_plugin< hive::plugins::chain::chain_plugin >().db();
    BOOST_REQUIRE( db );

    open_database();

    generate_block();
    db->set_hardfork( HIVE_NUM_HARDFORKS );
    generate_block();

    vest( "initminer", 10000 );

    // Fill up the rest of the required miners
    for( int i = HIVE_NUM_INIT_MINERS; i < HIVE_MAX_WITNESSES; i++ )
    {
      account_create( HIVE_INIT_MINER_NAME + fc::to_string( i ), init_account_pub_key );
      fund( HIVE_INIT_MINER_NAME + fc::to_string( i ), HIVE_MIN_PRODUCER_REWARD.amount.value );
      witness_create( HIVE_INIT_MINER_NAME + fc::to_string( i ), init_account_priv_key, "foo.bar", init_account_pub_key, HIVE_MIN_PRODUCER_REWARD.amount );
    }

    validate_database();

    ACTORS( (alice)(bob)(sam) );
    generate_block();

    fund( "alice", ASSET( "1000.000 HIVE" ) );
    fund( "alice", ASSET( "1000.000 TBD" ) );
    fund( "bob", ASSET( "1000.000 HIVE" ) );
    fund( "sam", ASSET( "1000.000 HIVE" ) );

    const auto& bucket_idx = db->get_index< bucket_index >().indices().get< by_bucket >();
    const auto& order_hist_idx = db->get_index< order_history_index >().indices().get< by_id >();

    BOOST_REQUIRE( bucket_idx.begin() == bucket_idx.end() );
    BOOST_REQUIRE( order_hist_idx.begin() == order_hist_idx.end() );
    validate_database();

    signed_transaction tx;

    auto fill_order_a_time = db->head_block_time();
    auto time_a = fc::time_point_sec( ( fill_order_a_time.sec_since_epoch() / 15 ) * 15 );

    limit_order_create_operation op;
    op.owner = "alice";
    op.amount_to_sell = ASSET( "1.000 TBD" );
    op.min_to_receive = ASSET( "2.000 HIVE" );
    op.expiration = db->head_block_time() + fc::seconds( HIVE_MAX_LIMIT_ORDER_EXPIRATION );
    tx.operations.push_back( op );
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    sign( tx, alice_private_key );
    db->push_transaction( tx,  0 );

    tx.operations.clear();
    tx.signatures.clear();

    op.owner = "bob";
    op.amount_to_sell = ASSET( "1.500 HIVE" );
    op.min_to_receive = ASSET( "0.750 TBD" );
    tx.operations.push_back( op );
    sign(tx, bob_private_key);
    db->push_transaction( tx, 0 );

    generate_blocks( db->head_block_time() + ( 60 * 90 ) );

    auto fill_order_b_time = db->head_block_time();

    tx.operations.clear();
    tx.signatures.clear();

    op.owner = "sam";
    op.amount_to_sell = ASSET( "1.000 HIVE" );
    op.min_to_receive = ASSET( "0.500 TBD" );
    tx.operations.push_back( op );
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    sign( tx, sam_private_key );
    db->push_transaction( tx, 0 );

    generate_blocks( db->head_block_time() + 60 );

    auto fill_order_c_time = db->head_block_time();

    tx.operations.clear();
    tx.signatures.clear();

    op.owner = "alice";
    op.amount_to_sell = ASSET( "0.500 TBD" );
    op.min_to_receive = ASSET( "0.900 HIVE" );
    tx.operations.push_back( op );
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    sign( tx, alice_private_key );
    db->push_transaction( tx, 0 );

    tx.operations.clear();
    tx.signatures.clear();

    op.owner = "bob";
    op.amount_to_sell = ASSET( "0.450 HIVE" );
    op.min_to_receive = ASSET( "0.250 TBD" );
    tx.operations.push_back( op );
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );
    sign( tx, bob_private_key );
    db->push_transaction( tx, 0 );
    validate_database();

    auto bucket = bucket_idx.begin();

    BOOST_REQUIRE( bucket->seconds == 15 );
    BOOST_REQUIRE( bucket->open == time_a );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "1.500 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "1.500 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.750 TBD" ).amount );

    bucket++;

    BOOST_REQUIRE( bucket->seconds == 15 );
    BOOST_REQUIRE( bucket->open == time_a + ( 60 * 90 ) );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "0.500 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "0.500 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.250 TBD" ).amount );

    bucket++;

    BOOST_REQUIRE( bucket->seconds == 15 );
    BOOST_REQUIRE( bucket->open == time_a + ( 60 * 90 ) + 60 );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "0.450 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "0.450 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "0.950 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.500 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket->seconds == 60 );
    BOOST_REQUIRE( bucket->open == time_a );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "1.500 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "1.500 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.750 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket->seconds == 60 );
    BOOST_REQUIRE( bucket->open == time_a + ( 60 * 90 ) );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "0.500 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "0.500 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.250 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket->seconds == 60 );
    BOOST_REQUIRE( bucket->open == time_a + ( 60 * 90 ) + 60 );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "0.450 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "0.450 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "0.950 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.500 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket->seconds == 300 );
    BOOST_REQUIRE( bucket->open == time_a );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "1.500 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "1.500 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.750 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket->seconds == 300 );
    BOOST_REQUIRE( bucket->open == time_a + ( 60 * 90 ) );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "0.450 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "0.450 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "1.450 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.750 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket->seconds == 3600 );
    BOOST_REQUIRE( bucket->open == time_a );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "1.500 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "1.500 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.750 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket->seconds == 3600 );
    BOOST_REQUIRE( bucket->open == time_a + ( 60 * 60 ) );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "0.450 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "0.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "0.450 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "1.450 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "0.750 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket->seconds == 86400 );
    BOOST_REQUIRE( bucket->open == HIVE_GENESIS_TIME );
    BOOST_REQUIRE( bucket->hive.high == ASSET( "0.450 TESTS " ).amount );
    BOOST_REQUIRE( bucket->hive.low == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.open == ASSET( "1.500 HIVE" ).amount );
    BOOST_REQUIRE( bucket->hive.close == ASSET( "0.450 HIVE").amount );
    BOOST_REQUIRE( bucket->hive.volume == ASSET( "2.950 HIVE" ).amount );
    BOOST_REQUIRE( bucket->non_hive.high == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.low == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.open == ASSET( "0.750 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.close == ASSET( "0.250 TBD" ).amount );
    BOOST_REQUIRE( bucket->non_hive.volume == ASSET( "1.500 TBD" ).amount );
    bucket++;

    BOOST_REQUIRE( bucket == bucket_idx.end() );

    auto order = order_hist_idx.begin();

    BOOST_REQUIRE( order->time == fill_order_a_time );
    BOOST_REQUIRE( order->op.current_owner == "bob" );
    BOOST_REQUIRE( order->op.current_orderid == 0 );
    BOOST_REQUIRE( order->op.current_pays == ASSET( "1.500 HIVE" ) );
    BOOST_REQUIRE( order->op.open_owner == "alice" );
    BOOST_REQUIRE( order->op.open_orderid == 0 );
    BOOST_REQUIRE( order->op.open_pays == ASSET( "0.750 TBD" ) );
    order++;

    BOOST_REQUIRE( order->time == fill_order_b_time );
    BOOST_REQUIRE( order->op.current_owner == "sam" );
    BOOST_REQUIRE( order->op.current_orderid == 0 );
    BOOST_REQUIRE( order->op.current_pays == ASSET( "0.500 HIVE" ) );
    BOOST_REQUIRE( order->op.open_owner == "alice" );
    BOOST_REQUIRE( order->op.open_orderid == 0 );
    BOOST_REQUIRE( order->op.open_pays == ASSET( "0.250 TBD" ) );
    order++;

    BOOST_REQUIRE( order->time == fill_order_c_time );
    BOOST_REQUIRE( order->op.current_owner == "alice" );
    BOOST_REQUIRE( order->op.current_orderid == 0 );
    BOOST_REQUIRE( order->op.current_pays == ASSET( "0.250 TBD" ) );
    BOOST_REQUIRE( order->op.open_owner == "sam" );
    BOOST_REQUIRE( order->op.open_orderid == 0 );
    BOOST_REQUIRE( order->op.open_pays == ASSET( "0.500 HIVE" ) );
    order++;

    BOOST_REQUIRE( order->time == fill_order_c_time );
    BOOST_REQUIRE( order->op.current_owner == "bob" );
    BOOST_REQUIRE( order->op.current_orderid == 0 );
    BOOST_REQUIRE( order->op.current_pays == ASSET( "0.450 HIVE" ) );
    BOOST_REQUIRE( order->op.open_owner == "alice" );
    BOOST_REQUIRE( order->op.open_orderid == 0 );
    BOOST_REQUIRE( order->op.open_pays == ASSET( "0.250 TBD" ) );
    order++;

    BOOST_REQUIRE( order == order_hist_idx.end() );
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()
#endif
