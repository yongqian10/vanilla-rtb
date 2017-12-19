#include <vector>
#include <random>
#include <utility>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/case_conv.hpp>

#include "rtb/core/core.hpp"
#include "rtb/exchange/exchange_handler.hpp"
#include "rtb/exchange/exchange_server.hpp"
#include "rtb/DSL/generic_dsl.hpp"
#include "rtb/DSL/any_mapper.hpp"
#include "rtb/DSL/rapid_mapper.hpp"
#include "rtb/config/config.hpp"
#include "rtb/core/tagged_tuple.hpp"
#include "rtb/datacache/entity_cache.hpp"
#include "rtb/datacache/memory_types.hpp"
#include "rtb/datacache/generic_bidder_cache_loader.hpp"
#include "CRUD/handlers/crud_dispatcher.hpp"


#include "ico_campaign.hpp"
#include "referer.hpp"
#include "examples/bidder/ad.hpp"

#include "bidder.hpp"
#include "ad_selector.hpp"


extern void init_framework_logging(const std::string &) ;


int main(int argc, char *argv[]) {
    using namespace std::placeholders;
    using namespace vanilla::exchange;
    using namespace std::chrono_literals;
    using restful_dispatcher_t =  http::crud::crud_dispatcher<http::server::request, http::server::reply> ;
    using DSLT = DSL::GenericDSL<std::string, DSL::rapid_mapper> ;
    using BidRequest = DSLT::deserialized_type;
    using BidResponse = DSLT::serialized_type;
    using BidderConfig = vanilla::config::config<ico_bidder_config_data>;
    using CacheLoader  =  vanilla::GenericBidderCacheLoader<RefererEntity<>, ICOCampaignEntity<>, AdDataEntity<BidderConfig>>;
    using Selector = vanilla::ad_selector<CacheLoader>;

    BidderConfig config([](ico_bidder_config_data &d, boost::program_options::options_description &desc){
        desc.add_options()
            ("ico_bidder.log", boost::program_options::value<std::string>(&d.log_file_name), "bidder_test log file name log")
            ("ico_bidder.ads_source", boost::program_options::value<std::string>(&d.ads_source)->default_value("data/ads"), "ads_source file name")
            ("ico_bidder.ads_ipc_name", boost::program_options::value<std::string>(&d.ads_ipc_name)->default_value("vanilla-ads-ipc"), "ads ipc name")
            ("bidder.referer_source", boost::program_options::value<std::string>(&d.referer_source)->default_value("data/referer"), "geo_source file name")
            ("bidder.referer_ipc_name", boost::program_options::value<std::string>(&d.referer_ipc_name)->default_value("vanilla-referer-ipc"), "referer ipc name")
            ("ico_bidder.port", boost::program_options::value<short>(&d.port)->required(), "ico_bidder port")
            ("ico_bidder.host", boost::program_options::value<std::string>(&d.host)->default_value("0.0.0.0"), "ico_bidder host")
            ("ico_bidder.root", boost::program_options::value<std::string>(&d.root)->default_value("."), "ico_bidder root")
            ("ico_bidder.timeout", boost::program_options::value<int>(&d.timeout), "ico_bidder timeout")
            ("ico_bidder.concurrency", boost::program_options::value<unsigned int>(&d.concurrency)->default_value(0), "ico_bidder concurrency, if 0 is set std::thread::hardware_concurrency()")
            ("ico_bidder.ico_campaign_ipc_name", boost::program_options::value<std::string>(&d.ico_campaign_ipc_name)->default_value("vanilla-ico-campaign-ipc"), "ico campaign ipc name")
            ("ico_bidder.ico_campaign_source", boost::program_options::value<std::string>(&d.ico_campaign_source)->default_value("data/ico_campaign"), "ico_campaign_source file name")
            ("campaign-manager.ipc_name", boost::program_options::value<std::string>(&d.ipc_name),"campaign_budget IPC name")
            ("campaign-manager.budget_source", boost::program_options::value<std::string>(&d.campaign_budget_source)->default_value("data/campaign_budget"),"campaign_budget source file name")
        ;
    });
    
    try {
        config.parse(argc, argv);
    }
    catch(std::exception const& e) {
        LOG(error) << e.what();
        return 0;
    }
    LOG(debug) << config;
    init_framework_logging(config.data().log_file_name);

    boost::uuids::random_generator uuid_generator{};
    CacheLoader cacheLoader(config);

    try {
        cacheLoader.load(); // Not needed if data cache loader is in work
    }
    catch(std::exception const& e) {
        LOG(error) << e.what();
        return 0;
    }
    
    using bid_handler_type = exchange_handler<DSLT, vanilla::UserInfo>;   

    bid_handler_type bid_handler(std::chrono::milliseconds(config.data().timeout));

    bid_handler    
        .logger([](const std::string &data) {
            //LOG(debug) << "bid request=" << data ;
        })
        .error_logger([](const std::string &data) {
            LOG(debug) << "bid request error " << data ;
        })
        .auction_async([&](const BidRequest &request) {
            thread_local vanilla::Bidder<DSLT, Selector> bidder(std::move(Selector(cacheLoader)));
            return bidder.bid(request);
        });
    
    connection_endpoint ep {std::make_tuple(config.data().host, boost::lexical_cast<std::string>(config.data().port), config.data().root)};

    //initialize and setup CRUD dispatcher
    restful_dispatcher_t dispatcher(ep.root);
    dispatcher.crud_match(boost::regex("/ico_bid/(\\d+)"))
            .post([&](http::server::reply & r, const http::crud::crud_match<boost::cmatch> & match) {
                bid_handler.handle_post(r, match);
            });

    LOG(debug) << "concurrency " << config.data().concurrency;
    exchange_server<restful_dispatcher_t> server{ep,dispatcher} ;
    server.set_concurrency(config.data().concurrency).run() ;
}


