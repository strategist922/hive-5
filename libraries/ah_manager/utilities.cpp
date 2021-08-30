
#include <vector>
#include <cstdint>

#include <hive/chain/util/impacted.hpp>

#include <fc/io/json.hpp>
#include <fc/string.hpp>

#include<map>

using hive::protocol::account_name_type;
using fc::string;

extern "C"
{

#ifdef elog
#pragma push_macro( "elog" )
#undef elog
#endif

#include <postgres.h>
#include <fmgr.h>
#include <catalog/pg_type.h>
#include <utils/builtins.h>
#include <utils/array.h>
#include <utils/lsyscache.h>
#include <funcapi.h>

#pragma pop_macro("elog")

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(get_impacted_accounts_array);
PG_FUNCTION_INFO_V1(get_impacted_accounts);
PG_FUNCTION_INFO_V1(clear_counters);
PG_FUNCTION_INFO_V1(get_counter);

flat_set<account_name_type> get_accounts( const std::string& operation_body )
{
  hive::protocol::operation _op;
  from_variant( fc::json::from_string( operation_body ), _op );

  flat_set<account_name_type> _impacted;
  hive::app::operation_get_impacted_accounts( _op, _impacted );

  return _impacted;
}

Datum get_impacted_accounts_array(PG_FUNCTION_ARGS)
{
  int16 typlen  = 0;
  bool typbyval = 0;
  char typalign = 0;

  get_typlenbyvalalign(TEXTOID, &typlen, &typbyval, &typalign);

  auto _construct_array = [&typlen, &typbyval, &typalign]( Datum* result = nullptr, int32_t size = 0 )
  {
    return construct_array(result, size, TEXTOID, typlen, typbyval, typalign);
  };

  try
  {
    Datum*     result = nullptr;

    VarChar* _arg0 = (VarChar*)PG_GETARG_VARCHAR_P(0);
    char* _op_body = (char*)VARDATA(_arg0);

    flat_set<account_name_type> _accounts = get_accounts( _op_body );

    if( _accounts.empty() )
    {
      PG_RETURN_POINTER( _construct_array() );
    }

    auto _size = _accounts.size();

    result = ( Datum* ) palloc( sizeof( Datum ) * _size );

    uint32_t cnt = 0;
    for( auto& acc : _accounts )
    {
      string _str = (string)acc;
      result[ cnt++ ] = CStringGetTextDatum( _str.c_str() );
    }

    PG_RETURN_POINTER( _construct_array(result, _size) );
  }
  catch(...)
  {
    PG_RETURN_POINTER( _construct_array() );
  }
}

Datum get_impacted_accounts(PG_FUNCTION_ARGS)
{
  FuncCallContext*  funcctx   = nullptr;

  try
  {
    int call_cntr = 0;
    int max_calls = 0;

    static Datum _empty = CStringGetTextDatum("");
    Datum current_account = _empty;

    bool _first_call = SRF_IS_FIRSTCALL();
    /* stuff done only on the first call of the function */
    if( _first_call )
    {
        MemoryContext   oldcontext;

        /* create a function context for cross-call persistence */
        funcctx = SRF_FIRSTCALL_INIT();

        /* switch to memory context appropriate for multiple function calls */
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* total number of tuples to be returned */
        VarChar* _arg0 = (VarChar*)PG_GETARG_VARCHAR_P(0);
        char* _op_body = (char*)VARDATA(_arg0);

        flat_set<account_name_type> _accounts = get_accounts( _op_body );

        funcctx->max_calls = _accounts.size();
        funcctx->user_fctx = nullptr;

        if( !_accounts.empty() )
        {
          auto itr = _accounts.begin();
          string _str = *(itr);
          current_account = CStringGetTextDatum( _str.c_str() );

          if( _accounts.size() > 1 )
          {
            Datum** _buffer = ( Datum** )palloc( sizeof( Datum* ) );
            for( size_t i = 1; i < _accounts.size(); ++i )
            {
              ++itr;
              _str = *(itr);

              _buffer[i - 1] = ( Datum* )palloc( sizeof( Datum ) );;
              *( _buffer[i - 1] ) = CStringGetTextDatum( _str.c_str() );
            }
            funcctx->user_fctx = _buffer;
          }
        }

        MemoryContextSwitchTo(oldcontext);
    }

    /* stuff done on every call of the function */
    funcctx = SRF_PERCALL_SETUP();

    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;

    if( call_cntr < max_calls )    /* do when there is more left to send */
    {
      if( !_first_call )
      {
        Datum** _buffer = ( Datum** )funcctx->user_fctx;
        current_account = *( _buffer[ call_cntr - 1 ] );
      }

      SRF_RETURN_NEXT(funcctx, current_account );
    }
    else    /* do when there is no more left */
    {
      if( funcctx->user_fctx )
      {
        Datum** _buffer = ( Datum** )funcctx->user_fctx;

        for( auto i = 0; i < max_calls - 1; ++i )
          pfree( _buffer[i] );

        pfree( _buffer );
      }

      SRF_RETURN_DONE(funcctx);
    }
  }
  catch(...)
  {
    SRF_RETURN_DONE(funcctx);
  }
}

using acc_counters_type = std::map<uint64_t, uint64_t>;
acc_counters_type acc_counters;

Datum clear_counters(PG_FUNCTION_ARGS)
{
  try
  {
    acc_counters.clear();
  }
  catch(...)
  {
  }
  PG_RETURN_UINT32( UInt32GetDatum(0) );
}

Datum get_counter(PG_FUNCTION_ARGS)
{
  try
  {
    uint32_t _account_id = PG_GETARG_INT64(0);
    uint32_t _current_cnt = PG_GETARG_INT64(1) + 1;

    auto found = acc_counters.find( _account_id );
    if( found == acc_counters.end() )
    {
      acc_counters.emplace( std::make_pair(_account_id, _current_cnt) );
    }
    else
    {
      ++( found->second );
      _current_cnt = found->second;
    }

    Datum _result = Int64GetDatum(_current_cnt);
    PG_RETURN_INT64(_result);
  }
  catch(...)
  {
  }
  PG_RETURN_INT64( Int32GetDatum(0) );
}

}