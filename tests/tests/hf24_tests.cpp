#ifdef IS_TEST_NET

#include <boost/test/unit_test.hpp>

#include <hive/chain/hive_fwd.hpp>

#include <hive/protocol/exceptions.hpp>
#include <hive/protocol/hardfork.hpp>
#include <hive/protocol/sps_operations.hpp>

#include <hive/chain/database.hpp>
#include <hive/chain/database_exceptions.hpp>
#include <hive/chain/hive_objects.hpp>

#include <hive/chain/util/reward.hpp>
#include <hive/chain/util/hf23_helper.hpp>

#include <hive/plugins/rc/rc_objects.hpp>
#include <hive/plugins/rc/resource_count.hpp>

#include <fc/macros.hpp>
#include <fc/crypto/digest.hpp>

#include "../db_fixture/database_fixture.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace hive;
using namespace hive::chain;
using namespace hive::protocol;
using fc::string;


BOOST_FIXTURE_TEST_SUITE( hf24_tests, hf24_database_fixture )

BOOST_AUTO_TEST_CASE( blocked_operations )
{
  try
  {
    BOOST_TEST_MESSAGE( "Even after HF24 steem.dao is considered a treasury account" );

    ACTORS( ( alice ) )
    generate_block();
    fund( "alice", ASSET( "10.000 HIVE" ) );
    fund( "alice", ASSET( "10.000 TBD" ) );
    generate_block();

    signed_transaction tx;
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );

    //check transfer of non-HBD
    {
      transfer_operation op;
      op.from = "alice";
      op.to = OBSOLETE_TREASURY_ACCOUNT;
      op.amount = ASSET( "1.000 HIVE" );
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception ); //still blocked even though old is no longer active treasury

      tx.clear();
      op.to = NEW_HIVE_TREASURY_ACCOUNT;
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception ); //blocked for new treasury as well
    }
    tx.clear();

    //check vesting of non-HBD
    {
      transfer_to_vesting_operation op;
      op.from = "alice";
      op.to = OBSOLETE_TREASURY_ACCOUNT;
      op.amount = ASSET( "1.000 HIVE" );
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );

      tx.clear();
      op.to = NEW_HIVE_TREASURY_ACCOUNT;
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );
    }
    tx.clear();

    //check withdraw vesting route
    {
      set_withdraw_vesting_route_operation op;
      op.from_account = "alice";
      op.to_account = OBSOLETE_TREASURY_ACCOUNT;
      op.percent = 50 * HIVE_1_PERCENT;
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );

      tx.clear();
      op.to_account = NEW_HIVE_TREASURY_ACCOUNT;
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );
    }
    tx.clear();

    //check transfer to savings
    {
      transfer_to_savings_operation op;
      op.from = "alice";
      op.to = OBSOLETE_TREASURY_ACCOUNT;
      op.amount = ASSET( "1.000 HIVE" );
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );

      tx.clear();
      op.amount = ASSET( "1.000 TBD" );
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );

      tx.clear();
      op.to = NEW_HIVE_TREASURY_ACCOUNT;
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );

      tx.clear();
      op.amount = ASSET( "1.000 HIVE" );
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );

      tx.clear();
      op.to = "alice";
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      db->push_transaction( tx, 0 );
      BOOST_REQUIRE( get_savings( "alice" ) == ASSET( "1.000 HIVE" ) );
    }
    tx.clear();

    //check transfer from savings of non-HBD
    {
      transfer_from_savings_operation op;
      op.from = "alice";
      op.to = OBSOLETE_TREASURY_ACCOUNT;
      op.amount = ASSET( "1.000 HIVE" );
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );

      tx.clear();
      op.to = NEW_HIVE_TREASURY_ACCOUNT;
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      BOOST_REQUIRE_THROW( db->push_transaction( tx, 0 ), fc::assert_exception );
    }
    tx.clear();

    database_fixture::validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( comment_beneficiary )
{
  try
  {
    BOOST_TEST_MESSAGE( "After HF24 steem.dao as comment beneficiary gives directly to new treasury account" );

    ACTORS( ( alice ) )
    generate_block();

    db_plugin->debug_update( []( database& db )
    {
      db.modify( db.get_dynamic_global_properties(), []( dynamic_global_property_object& gpo )
      {
        gpo.sps_fund_percent = 0;
      } );
    } );
    fund( "alice", ASSET( "10.000 HIVE" ) );
    fund( "alice", ASSET( "10.000 TBD" ) );
    generate_block();

    signed_transaction tx;
    tx.set_expiration( db->head_block_time() + HIVE_MAX_TIME_UNTIL_EXPIRATION );

    {
      comment_operation comment;
      comment.author = "alice";
      comment.permlink = "test";
      comment.parent_permlink = "test";
      comment.title = "test";
      comment.body = "Hello world";
      tx.operations.push_back( comment );
      sign( tx, alice_private_key );
      db->push_transaction( tx, 0 );
    }
    tx.clear();

    {
      comment_options_operation op;
      comment_payout_beneficiaries b;
      b.beneficiaries.push_back( beneficiary_route_type( OBSOLETE_TREASURY_ACCOUNT, HIVE_100_PERCENT ) );
      op.author = "alice";
      op.permlink = "test";
      op.allow_curation_rewards = false;
      op.extensions.insert( b );
      tx.operations.push_back( op );
      sign( tx, alice_private_key );
      db->push_transaction( tx, 0 );
    }
    tx.clear();

    {
      vote_operation vote;
      vote.author = "alice";
      vote.permlink = "test";
      vote.voter = "alice";
      vote.weight = HIVE_100_PERCENT;
      tx.operations.push_back( vote );
      sign( tx, alice_private_key );
      db->push_transaction( tx, 0 );
    }
    tx.clear();

    asset initial_treasury_balance = db->get_treasury().get_hbd_balance();
    generate_blocks( db->find_comment_cashout( db->get_comment( "alice", string( "test" ) ) )->cashout_time );
    BOOST_REQUIRE_EQUAL( get_hbd_balance( OBSOLETE_TREASURY_ACCOUNT ).amount.value, 0 );
    BOOST_REQUIRE_EQUAL( db->get_treasury().get_hbd_balance().amount.value, 1150 + initial_treasury_balance.amount.value );

    database_fixture::validate_database();
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_CASE( consolidate_balance )
{
  try
  {
    BOOST_TEST_MESSAGE( "After HF24 even if steem.dao gets some funds they will be transfered to new treasury account" );
    generate_block();
    
    //instead of trying to find a way to fund various balances of steem.dao, just write to them directly
    asset vested_3, vested_7;
    db_plugin->debug_update( [&]( database& db )
    {
      auto& dgpo = db.get_dynamic_global_properties();
      db.adjust_supply( ASSET( "20.000 HIVE" ) );
      db.adjust_supply( ASSET( "10.000 TBD" ) );
      vested_3 = ASSET( "3.000 HIVE" ) * dgpo.get_vesting_share_price();
      vested_7 = ASSET( "7.000 HIVE" ) * dgpo.get_vesting_share_price();
      db.modify( dgpo, []( dynamic_global_property_object& gpo )
      {
        gpo.sps_fund_percent = 0;
      } );
      auto& old_treasury = db.get_account( OBSOLETE_TREASURY_ACCOUNT );
      db.create_vesting( old_treasury, ASSET( "7.000 HIVE" ) );
      db.create_vesting( old_treasury, ASSET( "3.000 HIVE" ), true );
      db.modify( old_treasury, [&]( account_object& t )
      {
        t.balance = ASSET( "5.000 HIVE" );
        t.savings_balance = ASSET( "3.000 HIVE" );
        t.reward_hive_balance = ASSET( "2.000 HIVE" );
        t.hbd_balance = ASSET( "5.000 TBD" );
        t.savings_hbd_balance = ASSET( "3.000 TBD" );
        t.reward_hbd_balance = ASSET( "2.000 TBD" );
      } );
    } );
    database_fixture::validate_database();
    {
      auto& old_treasury = db->get_account( OBSOLETE_TREASURY_ACCOUNT );
      BOOST_REQUIRE_EQUAL( old_treasury.get_balance().amount.value, 5000 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_savings().amount.value, 3000 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_rewards().amount.value, 2000 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_hbd_balance().amount.value, 5000 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_hbd_savings().amount.value, 3000 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_hbd_rewards().amount.value, 2000 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_vesting().amount.value, vested_7.amount.value );
      BOOST_REQUIRE_EQUAL( old_treasury.get_vest_rewards().amount.value, vested_3.amount.value );
    }

    asset initial_treasury_balance = db->get_treasury().get_hbd_balance();
    generate_block();
    database_fixture::validate_database();

    {
      auto& old_treasury = db->get_account( OBSOLETE_TREASURY_ACCOUNT );
      BOOST_REQUIRE_EQUAL( old_treasury.get_balance().amount.value, 0 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_savings().amount.value, 0 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_rewards().amount.value, 0 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_hbd_balance().amount.value, 0 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_hbd_savings().amount.value, 0 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_hbd_rewards().amount.value, 0 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_vesting().amount.value, 0 );
      BOOST_REQUIRE_EQUAL( old_treasury.get_vest_rewards().amount.value, 0 );
    }
    {
      auto& treasury = db->get_account( NEW_HIVE_TREASURY_ACCOUNT );
      BOOST_REQUIRE_EQUAL( treasury.get_balance().amount.value, 14999 ); //rounding during vest->hive conversion
      BOOST_REQUIRE_EQUAL( treasury.get_savings().amount.value, 3000 );
      BOOST_REQUIRE_EQUAL( treasury.get_rewards().amount.value, 2000 );
      BOOST_REQUIRE_EQUAL( treasury.get_hbd_balance().amount.value, 5000 + initial_treasury_balance.amount.value );
      BOOST_REQUIRE_EQUAL( treasury.get_hbd_savings().amount.value, 3000 );
      BOOST_REQUIRE_EQUAL( treasury.get_hbd_rewards().amount.value, 2000 );
      BOOST_REQUIRE_EQUAL( treasury.get_vesting().amount.value, 0 );
      BOOST_REQUIRE_EQUAL( treasury.get_vest_rewards().amount.value, 0 );
    }
  }
  FC_LOG_AND_RETHROW()
}

BOOST_AUTO_TEST_SUITE_END()

#endif
