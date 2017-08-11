#include"backtestengine.h"
typedef StrategyTemplate*(*Dllfun)(AlgoTradingAPI*);
typedef int(*Release)();

BacktestEngine::BacktestEngine() :orderID(0), tradeCount(0)
{
	this->working_worker = 0;
	this->progressbarValue = 0;
	this->eventengine = new EventEngine;
	this->eventengine->regEvent(EVENT_BACKTEST_TICK, std::bind(&BacktestEngine::processTickEvent, this, std::placeholders::_1));
	this->eventengine->regEvent(EVENT_BACKTEST_BAR, std::bind(&BacktestEngine::processBarEvent, this, std::placeholders::_1));
	this->eventengine->regEvent(EVENT_LOG, std::bind(&BacktestEngine::showLog, this, std::placeholders::_1));
	this->eventengine->startEngine();
	//初始化MONGODB
	mongoc_init();													//1

	this->uri = mongoc_uri_new("mongodb://localhost:27017/");			//2

	this->pool = mongoc_client_pool_new(this->uri);							//3

	this->mongocxx = new MongoCxx(this->pool,true);
	readSymbolSize();
}

BacktestEngine::~BacktestEngine()
{
	delete this->eventengine;
	mongoc_client_pool_destroy(this->pool);
	mongoc_uri_destroy(this->uri);
	mongoc_cleanup();
	for (std::map<std::string, HINSTANCE>::const_iterator it = dllMap.begin(); it != dllMap.end(); ++it)
	{
		Release result = (Release)GetProcAddress(it->second, "ReleaseStrategy");//析构策略
		result();
	}
	delete mongocxx;
}

void BacktestEngine::clearStrategyObject()
{
	for (std::map<std::string, HINSTANCE>::const_iterator it = dllMap.begin(); it != dllMap.end(); ++it)
	{
		Release result = (Release)GetProcAddress(it->second, "ReleaseStrategy");//析构策略
		result();
	}
}

void BacktestEngine::loadStrategy()
{
	if (Utils::checkExist("./Strategy"))
	{
		std::fstream f("./Strategy/setting.json", std::ios::in);
		if (f.is_open())
		{
			std::stringstream buffer;
			buffer << f.rdbuf();
			std::string json = buffer.str();
			std::string err;
			const auto document = json11::Json::parse(json, err);
			if (!err.empty())
			{
				f.close();
				this->writeAlgoTradingLog("策略setting.json不是有效的json格式");
				return;
			}
			json11::Json::array strategyArray = document.array_items();
			for (int i = 0; i < strategyArray.size(); ++i)
			{
				//遍历每一个策略配置文件
				std::string strategyName = "./Strategy/" + strategyArray[i]["strategy"].string_value();
				std::vector<std::string>symbollist;
				json11::Json::array symbolArray = strategyArray[i]["symbol"].array_items();
				for (int j = 0; j < symbolArray.size(); ++j)
				{
					//遍历每一个策略中需要交易的合约
					symbollist.push_back(symbolArray[j].string_value());
				}
				std::map<std::string, std::string>paramMap;
				json11::Json::object paramObject = strategyArray[i]["param"].object_items();
				for (json11::Json::object::const_iterator it = paramObject.cbegin(); it != paramObject.cend(); ++it)
				{
					paramMap.insert(std::pair<std::string, std::string>(it->first, it->second.string_value()));
				}
				std::string param_symbol_name;
				for (std::vector<std::string>::const_iterator it = symbollist.cbegin(); it != symbollist.cend(); ++it)
				{
					param_symbol_name += (*it) + ",";
				}
				paramMap.insert(std::pair<std::string, std::string>("symbol", param_symbol_name));


#ifdef WIN32
				HINSTANCE his = LoadLibraryA(strategyName.c_str());//加载一个策略
				if (his == NULL)
				{
					//没有加载进来DLL
					this->writeAlgoTradingLog("无法读取策略" + strategyName);
					continue;
				}
				Dllfun dll = (Dllfun)GetProcAddress(his, "CreateStrategy");//创建策略
				if (dll == NULL)
				{
					//没有加载进来DLL
					this->writeAlgoTradingLog("无法调用CreateStrategy" + strategyName);
					FreeLibrary(his);
					continue;
				}
#endif

				//行情映射策略列表
				StrategyTemplate *strategy = dll(this);
				std::unique_lock<std::mutex>lck(quotes_mapping_strategymtx);
				for (std::vector<std::string>::const_iterator it = symbollist.cbegin(); it != symbollist.cend(); ++it)
				{
					if (this->quotes_mapping_strategy.find(*it) == this->quotes_mapping_strategy.end())
					{
						std::vector<StrategyTemplate*>tmp_strategylist;
						tmp_strategylist.push_back(strategy);
						this->quotes_mapping_strategy[(*it)] = tmp_strategylist;
					}
					else
					{
						this->quotes_mapping_strategy[*it].push_back(strategy);
					}
				}

				//参数
				for (std::map<std::string, std::string>::const_iterator it = paramMap.begin(); it != paramMap.end(); ++it)
				{
					strategy->addParam(it->first, it->second);
				}

				//持仓
				for (std::vector<std::string>::const_iterator it = symbollist.cbegin(); it != symbollist.cend(); ++it)
				{
					strategy->modifyPos((*it), 0);
				}
				std::unique_lock<std::mutex>lck2(name_mapping_strategymtx);
				this->name_mapping_strategy.insert(std::pair<std::string, StrategyTemplate*>(strategy->getParam("name"), strategy));
				this->dllMap.insert(std::pair<std::string, HINSTANCE>(strategy->getParam("name"), his));//策略名
				emit addStrategyItem(QString::fromStdString(strategy->getParam("name")));
			}
			f.close();
		}
		else
		{
			this->writeAlgoTradingLog("策略setting.json未能打开");
		}
	}
	else
	{
		this->writeAlgoTradingLog("没有Strategy目录");
	}
}

void BacktestEngine::readSymbolSize()
{
	std::fstream f("./size.csv", std::ios::in);
	if (f.is_open())
	{
		std::string line;
		while (getline(f, line))
		{
			std::vector<std::string>v;
			v = Utils::split(line, ",");
			if (v.size() == 2)
			{
				this->symbol_mapping_size.insert(std::pair<std::string, double>(v[0], std::stod(v[1])));
			}
		}
		f.close();
	}
}

void BacktestEngine::runBacktest(time_t startDateTime, time_t endDateTime, const QString &TickorBar, const SymbolStrategyNameMap &symbol_mapping_strategyname)
{
	this->writeAlgoTradingLog("确保目录下有size.csv");
	this->writeAlgoTradingLog("回测前先确定配置文件手续费合约代码和要回测的合约代码一致");

	this->startDatetime = startDateTime;
	clearStrategyObject();
	dllMap.clear();
	name_mapping_strategy.clear();

	loadStrategy();


	allOrders.clear();
	workingOrders.clear();
	holdingtradelist.clear();
	backtestResultMap.clear();
	plotgodstruct.barlist.clear();
	plotgodstruct.indicatorlist.clear();
	plotgodstruct.mainchartlist.clear();
	plotgodstruct.pnllist.clear();
	plotstruct.capital.clear();
	plotstruct.datetimelist.clear();
	plotstruct.drawdownlist.clear();
	quotes_mapping_strategy.clear();
	orderID_mapping_strategy.clear();
	DayMaxCapital.clear();


	std::set<std::string>backtest_symbols;
	for (std::map<std::string, std::vector<std::string>>::const_iterator iter = symbol_mapping_strategyname.cbegin(); iter != symbol_mapping_strategyname.cend(); ++iter)
	{
		std::vector<std::string>symbollist = Utils::split(iter->first, ",");
		for (std::vector<std::string>::const_iterator i = symbollist.cbegin(); i != symbollist.cend(); ++i)
		{
			const std::string symbol = *i;
			if (symbol != ""){
				backtest_symbols.insert(symbol);
				for (std::vector<std::string>::const_iterator it = symbol_mapping_strategyname.at(symbol).cbegin(); it != symbol_mapping_strategyname.at(symbol).cend(); ++it)
				{
					if (this->quotes_mapping_strategy.find(symbol) == this->quotes_mapping_strategy.end())
					{
						std::vector<StrategyTemplate*>v;
						this->quotes_mapping_strategy[symbol] = v;
					}
					this->quotes_mapping_strategy[symbol].push_back(this->name_mapping_strategy[*it]);
				}
			}
		}
	}
	if (backtest_symbols.empty())
	{
		this->writeAlgoTradingLog("没有添加策略你回测个毛线啊 (╬￣皿￣)＝○＃(￣＃)3￣)");
		return;
	}
	if (!Utils::checkExist("./traderecord"))
	{
		Utils::createDirectory("./traderecord");
	}
	Utils::deletedir("./traderecord");

	this->orderID = 0;
	this->tradeCount = 0;
	this->working_worker = 0;
	this->progressbarValue = 0;

	std::vector<std::shared_ptr<Event_Tick>>tick_vector;
	std::vector<jsstructs::BarData>bar_vector;
	clock_t startTime, endTime;
	startTime = clock();
	this->writeAlgoTradingLog(">>>提取历史数据");
	for (std::set<std::string>::const_iterator it = backtest_symbols.cbegin(); it != backtest_symbols.cend(); ++it)
	{
		if (TickorBar == "tick")
		{
			this->loadHistoryData(startDateTime, endDateTime, *it, tick_vector);
		}
		else if (TickorBar == "bar")
		{
			this->loadHistoryData(startDateTime, endDateTime, *it, bar_vector);
		}
	}
	this->writeAlgoTradingLog(">>>提取历史数据完成");
	this->writeAlgoTradingLog(">>>初始化需要回测的策略");
	std::unique_lock<std::mutex>lck(quotes_mapping_strategymtx);
	for (std::map<std::string, std::vector<StrategyTemplate*>>::const_iterator quotes_strategy_iter = this->quotes_mapping_strategy.begin(); quotes_strategy_iter != this->quotes_mapping_strategy.end(); ++quotes_strategy_iter)
	{
		for (std::vector<StrategyTemplate*>::const_iterator strategy_iter = quotes_strategy_iter->second.cbegin(); strategy_iter != quotes_strategy_iter->second.cend(); ++strategy_iter)
		{
			(*strategy_iter)->algorithmorder->setBacktestMode();
			(*strategy_iter)->onInit();
			std::vector<std::string>symbollist = Utils::split((*strategy_iter)->getParam("symbol"), ",");
			for (std::vector<std::string>::const_iterator symbol_iter = symbollist.cbegin(); symbol_iter != symbollist.cend(); ++symbol_iter)
			{
				if (*symbol_iter != "" && *symbol_iter != " ")
				{
					this->symbol_mapping_rate[Utils::regexSymbol(*symbol_iter)] = std::stod((*strategy_iter)->getParam(*symbol_iter));
					if ((*strategy_iter)->getParam("slippage_" + Utils::regexSymbol(*symbol_iter)) != "Null")
					{
						this->symbol_mapping_slippage[Utils::regexSymbol(*symbol_iter)] = std::stod((*strategy_iter)->getParam("slippage_" + Utils::regexSymbol(*symbol_iter)));
						this->writeAlgoTradingLog(*symbol_iter + "滑点为:" + (*strategy_iter)->getParam("slippage_" + Utils::regexSymbol(*symbol_iter)));
					}
					else
					{
						this->symbol_mapping_slippage[Utils::regexSymbol(*symbol_iter)] = 1;
						this->writeAlgoTradingLog(*symbol_iter + "未设置滑点，默认设置为1");
					}
				}
			}
		}
	}
	lck.unlock();
	this->writeAlgoTradingLog(">>>初始化完成");
	this->writeAlgoTradingLog(">>>启动各个策略");
	for (std::map<std::string, std::vector<StrategyTemplate*>>::const_iterator quotes_strategy_iter = this->quotes_mapping_strategy.cbegin(); quotes_strategy_iter != this->quotes_mapping_strategy.cend(); ++quotes_strategy_iter)
	{
		for (std::vector<StrategyTemplate*>::const_iterator strategy_iter = quotes_strategy_iter->second.cbegin(); strategy_iter != quotes_strategy_iter->second.cend(); ++strategy_iter)
		{
			(*strategy_iter)->trading = true;
			(*strategy_iter)->onStart();
		}
	}
	this->writeAlgoTradingLog(">>>启动完成");

	this->writeAlgoTradingLog(">>>开始数据排序并回放数据");
	emit setMaxProgressValue(std::max(bar_vector.size(), tick_vector.size()));

	if (TickorBar == "bar")
	{
		//排序
		std::sort(bar_vector.begin(), bar_vector.end(), BarGreater());
		if (!bar_vector.empty())
		{
			this->writeAlgoTradingLog("第一条数据的时间是:" + bar_vector.front().date + bar_vector.front().time);
			this->writeAlgoTradingLog("最后一条数据的时间是:" + bar_vector.back().date + bar_vector.back().time);
			for (std::vector<jsstructs::BarData>::const_iterator iter = bar_vector.cbegin(); iter != bar_vector.cend(); ++iter)
			{
				if (iter == bar_vector.begin())
				{
					this->synchronous_datetime = bar_vector.begin()->getTime_t();
				}
				//事件引擎写法
				std::shared_ptr<Event_Backtest_Bar>eBar = std::make_shared<Event_Backtest_Bar>();
				eBar->bar = (*iter);
				std::unique_lock<std::mutex>lck(working_workermtx);
				while (iter->getTime_t() != this->synchronous_datetime)
				{
					if (this->working_worker != 0)
					{
						this->workingworker_cv.wait(lck);
					}
					this->synchronous_datetime = iter->getTime_t();
					emit setProgressValue(this->progressbarValue);
				}
				++this->working_worker;
				this->PutEvent(eBar);
				++this->progressbarValue;
			}
			emit setProgressValue(this->progressbarValue);
		}
		else
		{
			this->writeAlgoTradingLog("木有数据你回测个毛啊！~");
		}
	}
	else if (TickorBar == "tick")
	{
		//排序
		std::sort(tick_vector.begin(), tick_vector.end(), TickGreater());
		if (!tick_vector.empty())
		{
			for (std::vector<std::shared_ptr<Event_Tick>>::const_iterator iter = tick_vector.begin(); iter != tick_vector.end(); ++iter)
			{
				if (iter == tick_vector.begin())
				{
					this->synchronous_datetime = (*tick_vector.begin())->getTime_t();
				}
				//事件引擎写法
				std::shared_ptr<Event_Backtest_Tick>eTick = std::make_shared<Event_Backtest_Tick>();
				eTick->tick = (*iter->get());
				std::unique_lock<std::mutex>lck(this->working_workermtx);
				while ((*iter)->getTime_t() != this->synchronous_datetime)
				{
					if (this->working_worker != 0)
					{
						this->workingworker_cv.wait(lck);
					}
					this->synchronous_datetime = (*iter)->getTime_t();
					emit setProgressValue(this->progressbarValue);
				}
				++this->working_worker;
				this->PutEvent(eTick);
				++this->progressbarValue;
			}
			emit setProgressValue(this->progressbarValue);
		}
		else
		{
			this->writeAlgoTradingLog("木有数据你回测个毛啊！~");
		}
	}
	endTime = clock();
	this->writeAlgoTradingLog(">>>回测完成,耗时" + std::to_string((double)(endTime - startTime) / CLOCKS_PER_SEC) + "s");
	while (true)
	{
		if (this->working_worker == 0)
		{
			break;
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}
	emit plotCapitalCurve(this->plotstruct);
	emit plotGodCurve(this->plotgodstruct);
}

void BacktestEngine::loadHistoryData(time_t startDateTime, time_t endDateTime, const std::string &symbol, std::vector<std::shared_ptr<Event_Tick>>&tick_vector)
{
	const char* databasename = "CTPTickDb";
	const char* collectionsname = symbol.c_str();

	mongoc_cursor_t *cursor;
	bson_error_t error;
	const bson_t *doc;
	mongoc_client_t    *client = mongoc_client_pool_pop(this->pool);																				//取一个mongo连接

	bson_t parent;
	bson_t child;
	mongoc_collection_t *collection;

	bson_init(&parent);
	BSON_APPEND_DOCUMENT_BEGIN(&parent, "datetime", &child);
	BSON_APPEND_TIME_T(&child, "$gt", startDateTime);
	BSON_APPEND_TIME_T(&child, "$lt", endDateTime);
	bson_append_document_end(&parent, &child);


	char * str = bson_as_json(&parent, NULL);
	//	printf("\n%s\n", str);

	collection = mongoc_client_get_collection(client, databasename, collectionsname);

	cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &parent, NULL, NULL);

	while (mongoc_cursor_next(cursor, &doc))
	{
		str = bson_as_json(doc, NULL);
		std::string s = str;
		std::string err;


		auto json = json11::Json::parse(s, err);
		if (!err.empty())
		{
			mongoc_cursor_destroy(cursor);
			mongoc_collection_destroy(collection);
			mongoc_client_pool_push(this->pool, client);																						//放回一个mongo连接
			return;
		}
		std::shared_ptr<Event_Tick>tick = std::make_shared<Event_Tick>();

		tick->symbol = json["symbol"].string_value();
		tick->exchange = json["exchange"].string_value();
		tick->gatewayname = json["gatewayname"].string_value();
		tick->lastprice = json["lastprice"].number_value();
		tick->volume = json["volume"].number_value();
		tick->openInterest = json["openInterest"].number_value();

		tick->date = json["date"].string_value();
		tick->time = json["time"].string_value();
		tick->setUnixDatetime();

		tick->openPrice = json["openPrice"].number_value();//今日开
		tick->highPrice = json["highPrice"].number_value();//今日高
		tick->lowPrice = json["lowPrice"].number_value();//今日低
		tick->preClosePrice = json["preClosePrice"].number_value();//昨收

		tick->upperLimit = json["upperLimit"].number_value();//涨停
		tick->lowerLimit = json["lowerLimit"].number_value();//跌停

		tick->bidprice1 = json["bidprice1"].number_value();
		tick->bidprice2 = json["bidprice2"].number_value();
		tick->bidprice3 = json["bidprice3"].number_value();
		tick->bidprice4 = json["bidprice4"].number_value();
		tick->bidprice5 = json["bidprice5"].number_value();

		tick->askprice1 = json["askprice1"].number_value();
		tick->askprice2 = json["askprice2"].number_value();
		tick->askprice3 = json["askprice3"].number_value();
		tick->askprice4 = json["askprice4"].number_value();
		tick->askprice5 = json["askprice5"].number_value();

		tick->bidvolume1 = json["bidvolume1"].number_value();
		tick->bidvolume2 = json["bidvolume2"].number_value();
		tick->bidvolume3 = json["bidvolume3"].number_value();
		tick->bidvolume4 = json["bidvolume4"].number_value();
		tick->bidvolume5 = json["bidvolume5"].number_value();

		tick->askvolume1 = json["askvolume1"].number_value();
		tick->askvolume2 = json["askvolume2"].number_value();
		tick->askvolume3 = json["askvolume3"].number_value();
		tick->askvolume4 = json["askvolume4"].number_value();
		tick->askvolume5 = json["askvolume5"].number_value();

		tick_vector.push_back(tick);
		bson_free(str);
	}

	if (mongoc_cursor_error(cursor, &error)) {
		this->writeAlgoTradingLog(error.message);
	}
	mongoc_cursor_destroy(cursor);
	mongoc_collection_destroy(collection);
	mongoc_client_pool_push(this->pool, client);																						//放回一个mongo连接
}

void BacktestEngine::loadHistoryData(time_t startDateTime, time_t endDateTime, const std::string &symbol, std::vector<jsstructs::BarData>&bar_vector)
{
	const char* databasename = "CTPMinuteDb";
	const char* collectionsname = symbol.c_str();

	mongoc_cursor_t *cursor;
	bson_error_t error;
	const bson_t *doc;

	bson_t parent;
	bson_t child;
	mongoc_collection_t *collection;

	bson_init(&parent);
	BSON_APPEND_DOCUMENT_BEGIN(&parent, "datetime", &child);
	BSON_APPEND_TIME_T(&child, "$gt", startDateTime);
	BSON_APPEND_TIME_T(&child, "$lt", endDateTime);
	bson_append_document_end(&parent, &child);

	char * str = bson_as_json(&parent, NULL);
	//	printf("\n%s\n", str);

	mongoc_client_t    *client = mongoc_client_pool_pop(this->pool);																				//取一个mongo连接

	collection = mongoc_client_get_collection(client, databasename, collectionsname);

	cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &parent, NULL, NULL);

	while (mongoc_cursor_next(cursor, &doc))
	{
		str = bson_as_json(doc, NULL);
		std::string s = str;
		std::string err;

		auto json = json11::Json::parse(s, err);
		if (!err.empty())
		{
			mongoc_cursor_destroy(cursor);
			mongoc_collection_destroy(collection);
			mongoc_client_pool_push(this->pool, client);																						//放回一个mongo连接
			return;
		}
		jsstructs::BarData bardata;
		bardata.symbol = json["symbol"].string_value();
		bardata.exchange = json["exchange"].string_value();
		bardata.open = json["open"].number_value();
		bardata.high = json["high"].number_value();
		bardata.low = json["low"].number_value();
		bardata.close = json["close"].number_value();
		bardata.volume = json["volume"].number_value();

		bardata.date = json["date"].string_value();
		bardata.time = json["time"].string_value();
		bardata.setUnixDatetime();

		bardata.openPrice = json["openPrice"].number_value();//今日开
		bardata.highPrice = json["highPrice"].number_value();//今日高
		bardata.lowPrice = json["lowPrice"].number_value();//今日低
		bardata.preClosePrice = json["preClosePrice"].number_value();//昨收

		bardata.upperLimit = json["upperLimit"].number_value();//涨停
		bardata.lowerLimit = json["lowerLimit"].number_value();//跌停

		bardata.openInterest = json["openInterest"].number_value();//持仓

		bar_vector.push_back(bardata);

		bson_free(str);
	}
	if (mongoc_cursor_error(cursor, &error)) {
		this->writeAlgoTradingLog(error.message);
	}
	mongoc_cursor_destroy(cursor);
	mongoc_collection_destroy(collection);
	mongoc_client_pool_push(this->pool, client);																						//放回一个mongo连接
}

void BacktestEngine::processTickEvent(std::shared_ptr<Event>e)
{
	std::shared_ptr<Event_Backtest_Tick> eTick = std::static_pointer_cast<Event_Backtest_Tick>(e);
	std::shared_ptr<Event_Tick>tick(&eTick->tick);
	this->crossLimitOrder(tick);

	std::unique_lock<std::mutex>lck(quotes_mapping_strategymtx);
	if (this->quotes_mapping_strategy.find(tick->symbol) != this->quotes_mapping_strategy.end())
	{
		for (std::vector<StrategyTemplate*>::const_iterator it = this->quotes_mapping_strategy[tick->symbol].cbegin(); it != this->quotes_mapping_strategy[tick->symbol].cend(); ++it)
		{
			(*it)->onTick_template(tick);
			this->recordPNL_Var(tick, *it);
		}
	}
	std::unique_lock<std::mutex>lck2(working_workermtx);
	this->working_worker -= 1;
	if (this->working_worker == 0)
	{
		this->workingworker_cv.notify_all();
	}

	if (tick->getHour() == 14 && tick->getMinute() == 59)
	{
		RecordCapital(tick);
	}
}

void BacktestEngine::processBarEvent(std::shared_ptr<Event>e)
{
	std::shared_ptr<Event_Backtest_Bar> eBar = std::static_pointer_cast<Event_Backtest_Bar>(e);
	jsstructs::BarData bar = eBar->bar;
	this->crossLimitOrder(bar);

	std::unique_lock<std::mutex>lck(quotes_mapping_strategymtx);
	if (this->quotes_mapping_strategy.find(bar.symbol) != this->quotes_mapping_strategy.end())
	{
		for (std::vector<StrategyTemplate*>::const_iterator it = this->quotes_mapping_strategy[bar.symbol].cbegin(); it != this->quotes_mapping_strategy[bar.symbol].cend(); ++it)
		{
			(*it)->onBar_template(bar,false);
			this->recordPNL_Var(bar, *it);
		}
	}
	std::unique_lock<std::mutex>lck2(working_workermtx);
	--this->working_worker;
	if (this->working_worker == 0)
	{
		this->workingworker_cv.notify_all();
	}

	if (bar.getHour() == 14 && bar.getMinute() == 59)
	{
		RecordCapital(bar);
	}
}

void BacktestEngine::crossLimitOrder(const std::shared_ptr<Event_Tick> &data)
{
	double	buyCrossPrice = data->askprice1;
	double	sellCrossPrice = data->bidprice1;
	double	buyBestCrossPrice = data->askprice1;
	double	sellBestCrossPrice = data->bidprice1;
	std::unique_lock<std::mutex>lck(ordersmtx);
	for (std::map<std::string, std::shared_ptr<Event_Order>>::iterator it = this->workingOrders.begin(); it != this->workingOrders.end();)
	{
		if (data->symbol == it->second->symbol)
		{
			bool buyCross = (it->second->direction == DIRECTION_LONG && it->second->price >= buyCrossPrice);
			bool sellCross = (it->second->direction == DIRECTION_SHORT && it->second->price <= sellCrossPrice);

			if (buyCross || sellCross)
			{
				++this->tradeCount;
				std::string tradeID = it->first;
				std::shared_ptr<Event_Trade>trade = std::make_shared<Event_Trade>();
				trade->symbol = it->second->symbol;
				trade->tradeID = tradeID;
				trade->orderID = it->second->orderID;
				trade->direction = it->second->direction;
				trade->offset = it->second->offset;
				trade->volume = it->second->totalVolume;
				trade->tradeTime = Utils::Time_ttoString(data->getTime_t());
				std::unique_lock<std::mutex>lck2(orderID_mapping_strategymtx);
				if (buyCross)
				{
					trade->price = std::min(it->second->price, buyBestCrossPrice);
					this->orderID_mapping_strategy[it->first]->modifyPos(it->second->symbol, this->orderID_mapping_strategy[it->first]->getPos(it->second->symbol) + trade->volume);
				}
				else if (sellCross)
				{
					trade->price = std::max(it->second->price, sellBestCrossPrice);
					this->orderID_mapping_strategy[it->first]->modifyPos(it->second->symbol, this->orderID_mapping_strategy[it->first]->getPos(it->second->symbol) - trade->volume);
				}
				this->orderID_mapping_strategy[it->first]->onTrade_template(trade);

				this->savetraderecord(this->orderID_mapping_strategy[trade->orderID]->getParam("name"), trade);

				this->settlement(trade);

				it->second->tradedVolume = it->second->totalVolume;
				it->second->status = STATUS_ALLTRADED;
				this->allOrders[it->first]->status = STATUS_ALLTRADED;

				this->orderID_mapping_strategy[it->first]->onOrder_template(it->second);
				this->workingOrders.erase(it++); //#1 

			}
			else
			{
				it++;
			}
		}
		else
		{
			it++;
		}
	}
}

void BacktestEngine::crossLimitOrder(const jsstructs::BarData &data)
{
	double	buyCrossPrice = data.low;
	double	sellCrossPrice = data.high;
	double	buyBestCrossPrice = data.open;
	double	sellBestCrossPrice = data.open;
	std::unique_lock<std::mutex>lck(ordersmtx);
	for (std::map<std::string, std::shared_ptr<Event_Order>>::iterator it = this->workingOrders.begin(); it != this->workingOrders.end();)
	{
		if (data.symbol == it->second->symbol)
		{
			bool buyCross = (it->second->direction == DIRECTION_LONG && it->second->price >= buyCrossPrice);
			bool sellCross = (it->second->direction == DIRECTION_SHORT && it->second->price <= sellCrossPrice);

			if (buyCross || sellCross)
			{
				++this->tradeCount;
				std::string tradeID = it->first;
				std::shared_ptr<Event_Trade>trade = std::make_shared<Event_Trade>();
				trade->symbol = it->second->symbol;
				trade->tradeID = tradeID;
				trade->orderID = it->second->orderID;
				trade->direction = it->second->direction;
				trade->offset = it->second->offset;
				trade->volume = it->second->totalVolume;
				trade->tradeTime = Utils::Time_ttoString(data.getTime_t());
				std::unique_lock<std::mutex>lck2(orderID_mapping_strategymtx);
				if (buyCross)
				{
					trade->price = std::min(it->second->price, buyBestCrossPrice);
					this->orderID_mapping_strategy[it->first]->modifyPos(it->second->symbol, this->orderID_mapping_strategy[it->first]->getPos(it->second->symbol) + trade->volume);
				}
				else if (sellCross)
				{
					trade->price = std::max(it->second->price, sellBestCrossPrice);
					this->orderID_mapping_strategy[it->first]->modifyPos(it->second->symbol, this->orderID_mapping_strategy[it->first]->getPos(it->second->symbol) - trade->volume);
				}
				this->orderID_mapping_strategy[it->first]->onTrade_template(trade);

				this->savetraderecord(this->orderID_mapping_strategy[trade->orderID]->getParam("name"), trade);

				this->settlement(trade);

				it->second->tradedVolume = it->second->totalVolume;
				it->second->status = STATUS_ALLTRADED;
				this->allOrders[it->first]->status = STATUS_ALLTRADED;

				this->orderID_mapping_strategy[it->first]->onOrder_template(it->second);
				this->workingOrders.erase(it++); //#1 

			}
			else
			{
				it++;
			}
		}
		else
		{
			it++;
		}
	}
}

void BacktestEngine::settlement(std::shared_ptr<Event_Trade>etrade)
{
	//清算
	if (this->orderID_mapping_strategy[etrade->orderID] == nullptr)
	{
		return;
	}

	const std::string strategyname = this->orderID_mapping_strategy[etrade->orderID]->getParam("name");
	std::vector<Event_Trade>longTrade_v;			//临时存储多头交易
	std::vector<Event_Trade>shortTrade_v;			//临时存储空头交易
	std::vector<TradingResult>resultList;			//清算列表

	std::unique_lock<std::mutex>lck(holdingtradelistmtx);
	if (this->holdingtradelist.find(strategyname) != this->holdingtradelist.end())//find current strategy's tradelist
	{
		if (this->holdingtradelist[strategyname].find(etrade->symbol) != this->holdingtradelist[strategyname].end())//find tradelist by symbol
		{
			for (std::vector<Event_Trade>::iterator tradeiter = this->holdingtradelist[strategyname][etrade->symbol].begin(); tradeiter != this->holdingtradelist[strategyname][etrade->symbol].end(); tradeiter++)
			{
				if ((tradeiter)->direction == DIRECTION_LONG)
				{
					longTrade_v.push_back(*tradeiter);
				}
				else
				{
					shortTrade_v.push_back(*tradeiter);
				}
			}
			this->holdingtradelist[strategyname][etrade->symbol].clear();
		}
	}

	if ((etrade)->direction == DIRECTION_LONG)
	{
		if (shortTrade_v.empty())
		{
			longTrade_v.push_back(*etrade);
		}
		else
		{
			Event_Trade exitTrade = *etrade;

			while (true)
			{
				Event_Trade *entryTrade = &shortTrade_v[0];

				//清算开平仓交易
				double closedVolume = std::min(exitTrade.volume, entryTrade->volume);
				TradingResult result = TradingResult(entryTrade->price, entryTrade->tradeTime, exitTrade.price, exitTrade.tradeTime, -closedVolume, symbol_mapping_rate[Utils::regexSymbol(entryTrade->symbol)], symbol_mapping_slippage[Utils::regexSymbol(entryTrade->symbol)], symbol_mapping_size[Utils::regexSymbol(etrade->symbol)]);
				resultList.push_back(result);

				//计算未清算部分
				entryTrade->volume -= closedVolume;
				exitTrade.volume -= closedVolume;

				//如果开仓交易经清算
				if (entryTrade->volume == 0)
				{
					shortTrade_v.erase(shortTrade_v.begin());
				}

				//如果平仓交易已经清算退出循环
				if (exitTrade.volume == 0)
				{
					break;
				}

				//如果平仓未全部清算
				if (exitTrade.volume)
				{
					if (shortTrade_v.empty())
					{
						longTrade_v.push_back(exitTrade);
						break;
					}
				}
			}
		}
	}
	//空头
	else
	{
		//如果尚无多头交易
		if (longTrade_v.empty())
		{
			shortTrade_v.push_back(*etrade);
		}
		else
		{
			Event_Trade exitTrade = *etrade;
			while (true)
			{
				Event_Trade *entryTrade = &longTrade_v[0];
				//清算开平仓交易
				double closedVolume = std::min(exitTrade.volume, entryTrade->volume);
				TradingResult result = TradingResult(entryTrade->price, entryTrade->tradeTime, exitTrade.price, entryTrade->tradeTime, closedVolume, symbol_mapping_rate[Utils::regexSymbol(entryTrade->symbol)], symbol_mapping_slippage[Utils::regexSymbol(entryTrade->symbol)], symbol_mapping_size[Utils::regexSymbol(etrade->symbol)]);
				resultList.push_back(result);

				//计算未清算部分

				entryTrade->volume -= closedVolume;
				exitTrade.volume -= closedVolume;

				//如果开仓交易已经全部清算，则从列表中移除
				if (entryTrade->volume == 0)
				{
					longTrade_v.erase(longTrade_v.begin());
				}

				//如果平仓交易已经全部清算，则退出循环
				if (exitTrade.volume == 0)
				{
					break;
				}
				//如果平仓交易未全部清算
				if (exitTrade.volume)
				{
					//且开仓交易已经全部清算完，则平仓交易剩余的部分
					// 等于新的反向开仓交易，添加到队列中
					if (longTrade_v.empty())
					{
						shortTrade_v.push_back(exitTrade);
						break;
					}
				}
			}
		}
	}
	//do open interest deals
	if (!shortTrade_v.empty())
	{
		//创建
		std::vector<Event_Trade>trade_v;
		for (std::vector<Event_Trade>::const_iterator shortTradeit = shortTrade_v.cbegin(); shortTradeit != shortTrade_v.cend(); ++shortTradeit)
		{
			trade_v.push_back(*shortTradeit);
		}


		if (this->holdingtradelist.find(strategyname) != this->holdingtradelist.end())
		{
			this->holdingtradelist[strategyname][etrade->symbol] = trade_v;
		}
		else
		{
			std::map<std::string, std::vector<Event_Trade>>symbol_vector;
			symbol_vector.insert(std::pair<std::string, std::vector<Event_Trade>>(etrade->symbol, trade_v));
			this->holdingtradelist.insert(std::pair<std::string, std::map<std::string, std::vector<Event_Trade>>>(strategyname, symbol_vector));
		}

	}
	if (!longTrade_v.empty())
	{
		std::vector<Event_Trade>trade_v;
		for (std::vector<Event_Trade>::const_iterator longTradeit = longTrade_v.cbegin(); longTradeit != longTrade_v.cend(); ++longTradeit)
		{
			trade_v.push_back(*longTradeit);
		}



		if (this->holdingtradelist.find(strategyname) != this->holdingtradelist.end())
		{
			this->holdingtradelist[strategyname][etrade->symbol] = trade_v;
		}
		else
		{
			std::map<std::string, std::vector<Event_Trade>>symbol_vector;
			symbol_vector.insert(std::pair<std::string, std::vector<Event_Trade>>(etrade->symbol, trade_v));
			this->holdingtradelist.insert(std::pair<std::string, std::map<std::string, std::vector<Event_Trade>>>(strategyname, symbol_vector));
		}
	}

	//计算每一笔结果
	std::unique_lock<std::mutex>lck2(backtestResultMapmtx);
	if (this->backtestResultMap.find(strategyname) == this->backtestResultMap.end())
	{
		Result res;
		UnitResult unit;
		res.insert(std::pair<std::string, UnitResult>(etrade->symbol, unit));
		this->backtestResultMap.insert(std::pair<std::string, Result>(strategyname, res));
	}

	if (this->backtestResultMap[strategyname].find(etrade->symbol) == this->backtestResultMap[strategyname].end())
	{
		//if arbitrage two symbol, may lost one symbol.
		UnitResult unit;
		this->backtestResultMap[strategyname][etrade->symbol] = unit;
	}
	//计算持仓
	double totalcost = 0;
	this->backtestResultMap[strategyname][etrade->symbol].holdingprice = 0;
	this->backtestResultMap[strategyname][etrade->symbol].holdingwinning = 0;
	this->backtestResultMap[strategyname][etrade->symbol].holdingposition = 0;
	for (std::vector<Event_Trade>::const_iterator symbol_tradememory_iter = this->holdingtradelist[strategyname][etrade->symbol].cbegin();
		symbol_tradememory_iter != this->holdingtradelist[strategyname][etrade->symbol].cend(); ++symbol_tradememory_iter)
	{
		totalcost += ((symbol_tradememory_iter)->volume) * ((symbol_tradememory_iter)->price);
		if ((symbol_tradememory_iter)->direction == DIRECTION_LONG)
		{
			this->backtestResultMap[strategyname][etrade->symbol].holdingposition += (symbol_tradememory_iter)->volume;
		}
		else if ((symbol_tradememory_iter)->direction == DIRECTION_SHORT)
		{
			this->backtestResultMap[strategyname][etrade->symbol].holdingposition -= (symbol_tradememory_iter)->volume;
		}
	}

	this->backtestResultMap[strategyname][etrade->symbol].holdingprice = totalcost / std::abs(this->backtestResultMap[strategyname][etrade->symbol].holdingposition);

	for (std::vector<TradingResult>::iterator resultiter = resultList.begin(); resultiter != resultList.end(); resultiter++)//settlement tradingresult
	{
		this->backtestResultMap[strategyname][etrade->symbol].totalwinning += resultiter->m_pnl;
		//this->backtestResultMap[strategyname][etrade->symbol].maxCapital = std::max(this->backtestResultMap[strategyname][etrade->symbol].totalwinning, this->backtestResultMap[strategyname][etrade->symbol].maxCapital);
		//this->backtestResultMap[strategyname][etrade->symbol].drawdown = this->backtestResultMap[strategyname][etrade->symbol].totalwinning - this->backtestResultMap[strategyname][etrade->symbol].maxCapital;
		if (resultiter->m_pnl >= 0)
		{
			this->backtestResultMap[strategyname][etrade->symbol].Winning += resultiter->m_pnl;
		}
		else
		{
			this->backtestResultMap[strategyname][etrade->symbol].Losing += resultiter->m_pnl;
		}
	}
}

void BacktestEngine::recordPNL_Var(const jsstructs::BarData &data, const StrategyTemplate *strategy)
{
	std::unique_lock<std::mutex>lck(backtestResultMapmtx);
	std::unique_lock<std::mutex>lck2(plotgodstructmtx);
	std::string strategyname = const_cast<StrategyTemplate*>(strategy)->getParam("name");
	if (plotgodstruct.barlist.find(strategyname) == plotgodstruct.barlist.end())
	{
		std::vector<jsstructs::BarData>v;
		plotgodstruct.barlist[strategyname] = v;
	}
	if (plotgodstruct.mainchartlist.find(strategyname) == plotgodstruct.mainchartlist.end())
	{
		std::map<std::string, std::vector<double>>v;
		plotgodstruct.mainchartlist[strategyname] = v;
	}
	if (plotgodstruct.indicatorlist.find(strategyname) == plotgodstruct.indicatorlist.end())
	{
		std::map<std::string, std::vector<double>>v;
		plotgodstruct.indicatorlist[strategyname] = v;
	}
	plotgodstruct.barlist[strategyname].push_back(data);
	std::map<std::string, double>indicator = strategy->backtestgoddata.indicatorMap;
	for (std::map<std::string, double>::const_iterator iter = indicator.cbegin(); iter != indicator.cend(); ++iter)
	{
		if (plotgodstruct.indicatorlist[strategyname].find(iter->first) == plotgodstruct.indicatorlist[strategyname].end())
		{
			std::vector<double>v;
			plotgodstruct.indicatorlist[strategyname][iter->first] = v;
		}
		plotgodstruct.indicatorlist[strategyname][iter->first].push_back(iter->second);
	}

	std::map<std::string, double>mainchart = strategy->backtestgoddata.mainchartMap;
	for (std::map<std::string, double>::const_iterator iter = mainchart.cbegin(); iter != mainchart.cend(); ++iter)
	{
		if (plotgodstruct.mainchartlist[strategyname].find(iter->first) == plotgodstruct.mainchartlist[strategyname].end())
		{
			std::vector<double>v;
			plotgodstruct.mainchartlist[strategyname][iter->first] = v;
		}
		plotgodstruct.mainchartlist[strategyname][iter->first].push_back(iter->second);
	}

	for (std::map<std::string, Result>::iterator it = this->backtestResultMap.begin(); it != this->backtestResultMap.end(); ++it)
	{
		const std::string strategyname = it->first;
		const std::string symbol = data.symbol;
		for (std::map<std::string, UnitResult>::iterator iter = it->second.begin(); iter != it->second.end(); ++iter)
		{
			//遍历所有合约
			if (iter->first == data.symbol)
			{
				if (iter->second.holdingposition > 0)
				{
					iter->second.holdingwinning = (data.close - iter->second.holdingprice)*iter->second.holdingposition*symbol_mapping_size[Utils::regexSymbol(data.symbol)];
				}
				else if (iter->second.holdingposition < 0)
				{
					iter->second.holdingwinning = (iter->second.holdingprice - data.close)*(-iter->second.holdingposition)*symbol_mapping_size[Utils::regexSymbol(data.symbol)];
				}
				else if (iter->second.holdingposition == 0)
				{
					iter->second.holdingwinning = 0;
				}
			}

			if (plotgodstruct.pnllist.find(strategyname) == plotgodstruct.pnllist.end())
			{
				std::vector<double>v(plotgodstruct.barlist[strategyname].size()-1,0);
				plotgodstruct.pnllist[strategyname] = v;
			}
			plotgodstruct.pnllist[strategyname].push_back(iter->second.holdingwinning + iter->second.totalwinning);
		}
	}
}

void BacktestEngine::recordPNL_Var(const std::shared_ptr<Event_Tick> &data, const StrategyTemplate*strategy)
{
	std::unique_lock<std::mutex>lck(backtestResultMapmtx);
	std::unique_lock<std::mutex>lck2(plotgodstructmtx);
	std::string strategyname = const_cast<StrategyTemplate*>(strategy)->getParam("name");
	if (plotgodstruct.mainchartlist.find(strategyname) == plotgodstruct.mainchartlist.end())
	{
		std::map<std::string, std::vector<double>>v;
		plotgodstruct.mainchartlist[strategyname] = v;
	}
	if (plotgodstruct.indicatorlist.find(strategyname) == plotgodstruct.indicatorlist.end())
	{
		std::map<std::string, std::vector<double>>v;
		plotgodstruct.indicatorlist[strategyname] = v;
	}
	if (plotgodstruct.mainchartlist[strategyname].find("ticklist") == plotgodstruct.mainchartlist[strategyname].end())
	{
		std::vector<double>v;
		plotgodstruct.mainchartlist[strategyname]["ticklist"] = v;
	}
	plotgodstruct.mainchartlist[strategyname]["ticklist"].push_back(data->lastprice);

	std::map<std::string, double>indicator = strategy->backtestgoddata.indicatorMap;
	for (std::map<std::string, double>::const_iterator iter = indicator.cbegin(); iter != indicator.cend(); ++iter)
	{
		if (plotgodstruct.indicatorlist[strategyname].find(iter->first) == plotgodstruct.indicatorlist[strategyname].end())
		{
			std::vector<double>v;
			plotgodstruct.indicatorlist[strategyname][iter->first] = v;
		}
		plotgodstruct.indicatorlist[strategyname][iter->first].push_back(iter->second);
	}

	std::map<std::string, double>mainchart = strategy->backtestgoddata.mainchartMap;
	for (std::map<std::string, double>::const_iterator iter = mainchart.cbegin(); iter != mainchart.cend(); ++iter)
	{
		if (plotgodstruct.mainchartlist[strategyname].find(iter->first) == plotgodstruct.mainchartlist[strategyname].end())
		{
			std::vector<double>v;
			plotgodstruct.mainchartlist[strategyname][iter->first] = v;
		}
		plotgodstruct.mainchartlist[strategyname][iter->first].push_back(iter->second);
	}

	for (std::map<std::string, Result>::iterator it = this->backtestResultMap.begin(); it != this->backtestResultMap.end(); ++it)
	{
		const std::string strategyname = it->first;
		const std::string symbol = data->symbol;
		for (std::map<std::string, UnitResult>::iterator iter = it->second.begin(); iter != it->second.end(); ++iter)
		{
			//遍历所有合约
			if (iter->first == data->symbol)
			{
				if (iter->second.holdingposition > 0)
				{
					iter->second.holdingwinning = (data->lastprice - iter->second.holdingprice)*iter->second.holdingposition*symbol_mapping_size[Utils::regexSymbol(data->symbol)];
				}
				else if (iter->second.holdingposition < 0)
				{
					iter->second.holdingwinning = (iter->second.holdingprice - data->lastprice)*(-iter->second.holdingposition)*symbol_mapping_size[Utils::regexSymbol(data->symbol)];
				}
				else if (iter->second.holdingposition == 0)
				{
					iter->second.holdingwinning = 0;
				}
				if (plotgodstruct.pnllist.find(strategyname) == plotgodstruct.pnllist.end())
				{
					std::vector<double>v(plotgodstruct.barlist[strategyname].size() - 1, 0);
					plotgodstruct.pnllist[strategyname] = v;
				}
				plotgodstruct.pnllist[strategyname].push_back(iter->second.totalwinning + iter->second.holdingwinning);
			}
		}
	}
}

void BacktestEngine::RecordCapital(const jsstructs::BarData &data)
{
	//这里m_result应该提早创建，要不然会发生第一天无法记录未交易合约的情况。
	std::unique_lock<std::mutex>lck(backtestResultMapmtx);
	std::unique_lock<std::mutex>lck2(plotstructmtx);

	for (std::map<std::string, Result>::iterator it = this->backtestResultMap.begin(); it != this->backtestResultMap.end(); ++it)
	{
		const std::string strategyname = it->first;
		const std::string symbol = data.symbol;
		for (std::map<std::string, UnitResult>::iterator iter = it->second.begin(); iter != it->second.end(); ++iter)
		{
			//遍历所有合约
			if (iter->first == data.symbol)
			{
				if (iter->second.holdingposition > 0)
				{
					iter->second.holdingwinning = (data.close - iter->second.holdingprice)*iter->second.holdingposition*symbol_mapping_size[Utils::regexSymbol(data.symbol)];
				}
				else if (iter->second.holdingposition < 0)
				{
					iter->second.holdingwinning = (iter->second.holdingprice - data.close)*(-iter->second.holdingposition)*symbol_mapping_size[Utils::regexSymbol(data.symbol)];
				}
				else if (iter->second.holdingposition == 0)
				{
					iter->second.holdingwinning = 0;
				}

				if (plotstruct.capital.find(strategyname) == plotstruct.capital.end())
				{
					std::vector<double>v;
					plotstruct.capital[strategyname] = v;
				}
				if (plotstruct.datetimelist.find(strategyname) == plotstruct.datetimelist.end())
				{
					std::vector<std::string>v;
					plotstruct.datetimelist[strategyname] = v;
				}
				if (plotstruct.drawdownlist.find(strategyname) == plotstruct.drawdownlist.end())
				{
					std::vector<double>v;
					plotstruct.drawdownlist[strategyname] = v;
				}

				if (DayMaxCapital.find(strategyname) == DayMaxCapital.end())
				{
					std::map<std::string, double>map;
					DayMaxCapital[strategyname] = map;
				}

				if (DayMaxCapital[strategyname].find(symbol) == DayMaxCapital[strategyname].end())
				{
					DayMaxCapital[strategyname][symbol] = iter->second.holdingwinning + iter->second.totalwinning;
				}

				DayMaxCapital[strategyname][symbol] = std::max(DayMaxCapital[strategyname][symbol], iter->second.holdingwinning + iter->second.totalwinning);

				plotstruct.capital[strategyname].push_back(iter->second.holdingwinning + iter->second.totalwinning);
				plotstruct.drawdownlist[strategyname].push_back(iter->second.holdingwinning + iter->second.totalwinning - DayMaxCapital[strategyname][symbol]);
				plotstruct.datetimelist[strategyname].push_back(data.date + data.time);
			}
		}
	}
}

void BacktestEngine::RecordCapital(const std::shared_ptr<Event_Tick> &data)
{
	std::unique_lock<std::mutex>lck(backtestResultMapmtx);
	std::unique_lock<std::mutex>lck2(plotstructmtx);

	for (std::map<std::string, Result>::iterator it = this->backtestResultMap.begin(); it != this->backtestResultMap.end(); ++it)
	{
		const std::string strategyname = it->first;
		const std::string symbol = data->symbol;
		for (std::map<std::string, UnitResult>::iterator iter = it->second.begin(); iter != it->second.end(); ++iter)
		{
			//遍历所有合约
			if (iter->first == data->symbol)
			{
				if (iter->second.holdingposition > 0)
				{
					iter->second.holdingwinning = (data->lastprice - iter->second.holdingprice)*iter->second.holdingposition*symbol_mapping_size[Utils::regexSymbol(data->symbol)];
				}
				else if (iter->second.holdingposition < 0)
				{
					iter->second.holdingwinning = (iter->second.holdingprice - data->lastprice)*(-iter->second.holdingposition)*symbol_mapping_size[Utils::regexSymbol(data->symbol)];
				}
				else if (iter->second.holdingposition == 0)
				{
					iter->second.holdingwinning = 0;
				}

				if (plotstruct.capital.find(strategyname) == plotstruct.capital.end())
				{
					std::vector<double>v;
					plotstruct.capital[strategyname] = v;
				}
				if (plotstruct.datetimelist.find(strategyname) == plotstruct.datetimelist.end())
				{
					std::vector<std::string>v;
					plotstruct.datetimelist[strategyname] = v;
				}
				if (plotstruct.drawdownlist.find(strategyname) == plotstruct.drawdownlist.end())
				{
					std::vector<double>v;
					plotstruct.drawdownlist[strategyname] = v;
				}

				if (DayMaxCapital.find(strategyname) == DayMaxCapital.end())
				{
					std::map<std::string, double>map;
					DayMaxCapital[strategyname] = map;
				}

				if (DayMaxCapital[strategyname].find(symbol) == DayMaxCapital[strategyname].end())
				{
					DayMaxCapital[strategyname][symbol] = iter->second.holdingwinning + iter->second.totalwinning;
				}

				DayMaxCapital[strategyname][symbol] = std::max(DayMaxCapital[strategyname][symbol], iter->second.holdingwinning + iter->second.totalwinning);

				plotstruct.capital[strategyname].push_back(iter->second.holdingwinning+iter->second.totalwinning);
				plotstruct.drawdownlist[strategyname].push_back(iter->second.holdingwinning + iter->second.totalwinning - DayMaxCapital[strategyname][symbol]);
				plotstruct.datetimelist[strategyname].push_back(data->date + data->time);
			}
		}
	}
}

void BacktestEngine::savetraderecord(std::string strategyname, std::shared_ptr<Event_Trade>etrade)
{
	if (Utils::checkExist("./traderecord"))
	{
		std::fstream f;
		f.open("./traderecord/" + strategyname + ".csv", std::ios::app | std::ios::out);
		if (f.is_open())
		{
			std::string symbol = etrade->symbol;
			std::string direction = etrade->direction;
			std::string offset = etrade->offset;
			std::string tradetime = etrade->tradeTime;
			std::string volume = std::to_string(etrade->volume);
			std::string price = std::to_string(etrade->price);
			f << strategyname << "," << tradetime << "," << symbol << "," << direction << "," << offset << "," << price << "," << volume << "\n";
			f.close();
		}
	}
}

std::vector<std::string> BacktestEngine::sendOrder(const std::string &symbol, const std::string &orderType, double price, double volume, StrategyTemplate* Strategy)
{
	std::unique_lock<std::mutex>lck1(ordersmtx);
	std::unique_lock<std::mutex>lck2(orderID_mapping_strategymtx);
	std::shared_ptr<Event_Order> req = std::make_shared<Event_Order>();
	req->symbol = symbol;
	req->price = price;
	req->totalVolume = volume;
	req->status = STATUS_WAITING;
	if (orderType == ALGOBUY)
	{
		req->direction = DIRECTION_LONG;//做多
		req->offset = OFFSET_OPEN;//开仓
		std::string orderID = std::to_string(this->orderID++); //
		req->orderID = orderID;
		this->workingOrders.insert(std::pair<std::string, std::shared_ptr<Event_Order>>(orderID, req));
		this->allOrders.insert(std::pair<std::string, std::shared_ptr<Event_Order>>(orderID, req));
		this->orderID_mapping_strategy.insert(std::pair<std::string, StrategyTemplate*>(orderID, Strategy));
		std::vector<std::string>result;
		result.push_back(orderID);
		return result;
	}
	else if (orderType == ALGOSELL)
	{
		req->direction = DIRECTION_SHORT;//平多
		req->offset = OFFSET_CLOSE;
		std::string orderID = std::to_string(this->orderID++); //
		req->orderID = orderID;
		this->workingOrders.insert(std::pair<std::string, std::shared_ptr<Event_Order>>(orderID, req));
		this->allOrders.insert(std::pair<std::string, std::shared_ptr<Event_Order>>(orderID, req));
		this->orderID_mapping_strategy.insert(std::pair<std::string, StrategyTemplate*>(orderID, Strategy));
		std::vector<std::string>result;
		result.push_back(orderID);
		return result;
	}
	else if (orderType == ALGOSHORT)
	{
		req->direction = DIRECTION_SHORT;
		req->offset = OFFSET_OPEN;
		std::string orderID = std::to_string(this->orderID++); //
		req->orderID = orderID;
		this->workingOrders.insert(std::pair<std::string, std::shared_ptr<Event_Order>>(orderID, req));
		this->allOrders.insert(std::pair<std::string, std::shared_ptr<Event_Order>>(orderID, req));
		this->orderID_mapping_strategy.insert(std::pair<std::string, StrategyTemplate*>(orderID, Strategy));
		std::vector<std::string>result;
		result.push_back(orderID);
		return result;
	}
	else if (orderType == ALGOCOVER)
	{
		req->direction = DIRECTION_LONG;
		req->offset = OFFSET_CLOSE;
		std::string orderID = std::to_string(this->orderID++); //
		req->orderID = orderID;
		this->workingOrders.insert(std::pair<std::string, std::shared_ptr<Event_Order>>(orderID, req));
		this->allOrders.insert(std::pair<std::string, std::shared_ptr<Event_Order>>(orderID, req));
		this->orderID_mapping_strategy.insert(std::pair<std::string, StrategyTemplate*>(orderID, Strategy));
		std::vector<std::string>result;
		result.push_back(orderID);
		return result;
	}
}

void BacktestEngine::cancelOrder(const std::string &orderID, const std::string &gatewayName)
{
	std::unique_lock<std::mutex>lck(ordersmtx);
	std::unique_lock<std::mutex>lck2(orderID_mapping_strategymtx);
	if (this->workingOrders.find(orderID) != this->workingOrders.end())
	{
		std::shared_ptr<Event_Order>order = this->workingOrders[orderID];
		if (order != nullptr)
		{
			if (!(order->status == STATUS_ALLTRADED || order->status == STATUS_CANCELLED))
			{
				this->allOrders[orderID]->status = STATUS_CANCELLED;
				this->workingOrders.erase(orderID);
				this->orderID_mapping_strategy[orderID]->onOrder_template(order);
			}
		}
	}
}

void BacktestEngine::writeAlgoTradingLog(const std::string &msg)
{
	std::shared_ptr<Event_Log>e = std::make_shared<Event_Log>();
	e->msg = msg;
	e->gatewayname = "BacktestEngine";
	e->logTime = Utils::getCurrentDateTime();
	eventengine->put(e);
}

void BacktestEngine::writeTradingReason(const std::shared_ptr<Event_Tick> &tick, const std::string &msg, StrategyTemplate *strategy)
{
	std::shared_ptr<Event_Log>e = std::make_shared<Event_Log>();
	e->msg = "策略:" + strategy->getParam("name") + " " + msg;
	e->gatewayname = "BacktestEngine";
	e->logTime = tick->date + " " + tick->time;
	this->eventengine->put(e);
}

void BacktestEngine::writeTradingReason(const jsstructs::BarData &bar, const std::string &msg, StrategyTemplate *strategy)
{
	std::shared_ptr<Event_Log>e = std::make_shared<Event_Log>();
	e->msg = "策略:" + strategy->getParam("name") + " " + msg;
	e->gatewayname = "BacktestEngine";
	e->logTime = bar.date + " " + bar.time;
	this->eventengine->put(e);
}

void BacktestEngine::showLog(std::shared_ptr<Event>e)
{
	std::shared_ptr<Event_Log> elog = std::static_pointer_cast<Event_Log>(e);
	std::string msg = ">>>接口名:" + elog->gatewayname + "时间:" + elog->logTime + "信息:" + elog->msg;
	emit sendMSG(QString::fromStdString(msg));
}

void BacktestEngine::PutEvent(std::shared_ptr<Event>e)
{
	this->eventengine->put(e);
}

std::vector<std::shared_ptr<Event_Tick>>BacktestEngine::loadTick(const std::string &tickDbName, const std::string &symbol, int days)
{
	std::vector<std::shared_ptr<Event_Tick>>datavector;
	if (symbol == " " || symbol == "")
	{
		return datavector;
	}
	const char* databasename = tickDbName.c_str();
	const char* collectionsname = symbol.c_str();
	auto targetday = startDatetime - (days * 24 * 3600);//获取当前的系统时间


	mongoc_cursor_t *cursor;
	bson_error_t error;
	const bson_t *doc;


	// 从客户端池中获取一个客户端
	mongoc_client_t      *client = mongoc_client_pool_pop(this->pool);																//取一个mongo连接

	bson_t parent;
	bson_t child;
	mongoc_collection_t *collection;
	bson_init(&parent);
	BSON_APPEND_DOCUMENT_BEGIN(&parent, "datetime", &child);
	BSON_APPEND_TIME_T(&child, "$gt", targetday);
	BSON_APPEND_TIME_T(&child, "$lt", startDatetime);
	bson_append_document_end(&parent, &child);


	char * str = bson_as_json(&parent, NULL);
	//	printf("\n%s\n", str);

	collection = mongoc_client_get_collection(client, tickDbName.c_str(), symbol.c_str());

	cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &parent, NULL, NULL);

	while (mongoc_cursor_next(cursor, &doc)) {
		str = bson_as_json(doc, NULL);
		std::string s = str;
		std::string err;


		auto json = json11::Json::parse(s, err);
		if (!err.empty())
		{
			mongoc_cursor_destroy(cursor);
			mongoc_collection_destroy(collection);
			mongoc_client_pool_push(this->pool, client);																						//放回一个mongo连接
			return datavector;
		}
		std::shared_ptr<Event_Tick>tickdata = std::make_shared<Event_Tick>();

		tickdata->symbol = json["symbol"].string_value();
		tickdata->exchange = json["exchange"].string_value();
		tickdata->gatewayname = json["gatewayname"].string_value();

		tickdata->lastprice = json["lastprice"].number_value();
		tickdata->volume = json["volume"].number_value();
		tickdata->openInterest = json["openInterest"].number_value();

		tickdata->date = json["date"].string_value();
		tickdata->time = json["time"].string_value();
		tickdata->setUnixDatetime();

		tickdata->openPrice = json["openPrice"].number_value();//今日开
		tickdata->highPrice = json["highPrice"].number_value();//今日高
		tickdata->lowPrice = json["lowPrice"].number_value();//今日低
		tickdata->preClosePrice = json["preClosePrice"].number_value();//昨收

		tickdata->upperLimit = json["upperLimit"].number_value();//涨停
		tickdata->lowerLimit = json["lowerLimit"].number_value();//跌停

		tickdata->bidprice1 = json["bidprice1"].number_value();
		tickdata->bidprice2 = json["bidprice2"].number_value();
		tickdata->bidprice3 = json["bidprice3"].number_value();
		tickdata->bidprice4 = json["bidprice4"].number_value();
		tickdata->bidprice5 = json["bidprice5"].number_value();

		tickdata->askprice1 = json["askprice1"].number_value();
		tickdata->askprice2 = json["askprice2"].number_value();
		tickdata->askprice3 = json["askprice3"].number_value();
		tickdata->askprice4 = json["askprice4"].number_value();
		tickdata->askprice5 = json["askprice5"].number_value();

		tickdata->bidvolume1 = json["bidvolume1"].number_value();
		tickdata->bidvolume2 = json["bidvolume2"].number_value();
		tickdata->bidvolume3 = json["bidvolume3"].number_value();
		tickdata->bidvolume4 = json["bidvolume4"].number_value();
		tickdata->bidvolume5 = json["bidvolume5"].number_value();

		tickdata->askvolume1 = json["askvolume1"].number_value();
		tickdata->askvolume2 = json["askvolume2"].number_value();
		tickdata->askvolume3 = json["askvolume3"].number_value();
		tickdata->askvolume4 = json["askvolume4"].number_value();
		tickdata->askvolume5 = json["askvolume5"].number_value();

		datavector.push_back(tickdata);

		//		printf("%s\n", str);
		bson_free(str);
	}

	if (mongoc_cursor_error(cursor, &error)) {
		fprintf(stderr, "An error occurred: %s\n", error.message);
	}
	mongoc_cursor_destroy(cursor);
	mongoc_collection_destroy(collection);
	mongoc_client_pool_push(this->pool, client);																						//放回一个mongo连接
	return datavector;
}

std::vector<jsstructs::BarData>BacktestEngine::loadBar(const std::string &BarDbName, const std::string &symbol, int days)
{
	std::vector<jsstructs::BarData>datavector;
	if (symbol == " " || symbol == "")
	{
		return datavector;
	}
	const char* databasename = BarDbName.c_str();
	const char* collectionsname = symbol.c_str();
	auto targetday = startDatetime - (days * 24 * 3600);//获取当前的系统时间

	mongoc_cursor_t *cursor;
	bson_error_t error;
	const bson_t *doc;


	bson_t parent;
	bson_t child;
	mongoc_collection_t *collection;
	bson_init(&parent);
	BSON_APPEND_DOCUMENT_BEGIN(&parent, "datetime", &child);
	BSON_APPEND_TIME_T(&child, "$gt", targetday);
	BSON_APPEND_TIME_T(&child, "$lt", startDatetime);
	bson_append_document_end(&parent, &child);


	char * str = bson_as_json(&parent, NULL);
	//	printf("\n%s\n", str);

	// 从客户端池中获取一个客户端
	mongoc_client_t    *client = mongoc_client_pool_pop(this->pool);																				//取一个mongo连接

	collection = mongoc_client_get_collection(client, BarDbName.c_str(), symbol.c_str());

	cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 0, 0, &parent, NULL, NULL);

	while (mongoc_cursor_next(cursor, &doc))
	{
		str = bson_as_json(doc, NULL);
		std::string s = str;
		std::string err;


		auto json = json11::Json::parse(s, err);
		if (!err.empty())
		{
			mongoc_cursor_destroy(cursor);
			mongoc_collection_destroy(collection);
			mongoc_client_pool_push(this->pool, client);																						//放回一个mongo连接
			return datavector;
		}
		jsstructs::BarData bardata;
		bardata.symbol = json["symbol"].string_value();
		bardata.exchange = json["exchange"].string_value();
		bardata.open = json["open"].number_value();
		bardata.high = json["high"].number_value();
		bardata.low = json["low"].number_value();
		bardata.close = json["close"].number_value();
		bardata.volume = json["volume"].number_value();

		bardata.date = json["date"].string_value();
		bardata.time = json["time"].string_value();
		bardata.setUnixDatetime();

		bardata.openPrice = json["openPrice"].number_value();//今日开
		bardata.highPrice = json["highPrice"].number_value();//今日高
		bardata.lowPrice = json["lowPrice"].number_value();//今日低
		bardata.preClosePrice = json["preClosePrice"].number_value();//昨收

		bardata.upperLimit = json["upperLimit"].number_value();//涨停
		bardata.lowerLimit = json["lowerLimit"].number_value();//跌停

		bardata.openInterest = json["openInterest"].number_value();//持仓

		datavector.push_back(bardata);

		//		printf("%s\n", str);
		bson_free(str);
	}

	if (mongoc_cursor_error(cursor, &error)) {
		fprintf(stderr, "An error occurred: %s\n", error.message);
	}

	mongoc_cursor_destroy(cursor);
	mongoc_collection_destroy(collection);
	mongoc_client_pool_push(this->pool, client);																						//放回一个mongo连接
	return datavector;
}

std::string BacktestEngine::getParam(const std::string &strategyname, const std::string &param)
{
	std::unique_lock<std::mutex>lck(name_mapping_strategymtx);
	return this->name_mapping_strategy[strategyname]->getParam(param);
}