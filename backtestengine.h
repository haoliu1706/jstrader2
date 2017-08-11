#ifndef BACKTESTENGINE_H
#define BACKTESTENGINE_H
#include<QObject>
#include<QStandardItemModel>
#include<fstream>
#include<string>
#include<set>
#include<ctime>
#include"json11/json11.h"
#include"algotradingAPI.h"
#include"strategyTemplate.h"
#include"eventengine.h"
#include"structs.hpp"
typedef std::map<std::string, std::vector<std::string>> SymbolStrategyNameMap;
struct PlotGodStruct
{
	std::map<std::string, std::vector<jsstructs::BarData>>barlist;													//key是策略 Value是K线列
	std::map<std::string, std::vector<double>>pnllist;											//在backtestengine中记录Bar级别盈亏
	std::map<std::string, std::map<std::string, std::vector<double>>>mainchartlist;				//key是策略Value是一个map，key是变量名，value是值的序列
	std::map<std::string, std::map<std::string, std::vector<double>>>indicatorlist;				//key是策略Value是一个map，key是变量名，value是值的序列
};

struct PlotStruct
{
	std::map<std::string, std::vector<std::string>>datetimelist;											//在backtestengine中记录Bar级别盈亏
	std::map<std::string, std::vector<double>>capital;											//在backtestengine中记录Bar级别盈亏
	std::map<std::string, std::vector<double>>drawdownlist;											//在backtestengine中记录Bar级别盈亏
};

class TradingResult
{
public:
	TradingResult(double entryPrice, std::string entryDt, double exitPrice, std::string exitDt, double volume, double rate, double slippage, double size)
	{
		//清算
		m_entryPrice = entryPrice;  //开仓价格
		m_exitPrice = exitPrice;    // 平仓价格
		m_entryDt = entryDt;        // 开仓时间datetime
		m_exitDt = exitDt;          // 平仓时间
		m_volume = volume;			//交易数量（ + / -代表方向）
		m_turnover = (entryPrice + exitPrice)*size*abs(volume);   //成交金额
		m_commission = m_turnover*rate;                                // 手续费成本
		m_slippage = slippage * 2 * size*abs(volume);                        // 滑点成本
		m_pnl = ((m_exitPrice - m_entryPrice) * volume * size - m_commission - m_slippage);  //净盈亏
	};
	double m_entryPrice;  //开仓价格
	double m_exitPrice;   // 平仓价格
	std::string m_entryDt;   // 开仓时间datetime
	std::string  m_exitDt;  // 平仓时间
	double	m_volume;//交易数量（ + / -代表方向）
	double	m_pnl;  //净盈亏
	double m_turnover;
	double m_commission;
	double m_slippage;
};

struct UnitResult
{
	double  totalwinning = 0;
	double	maxCapital = 0;
	double	drawdown = 0;
	double	Winning = 0;
	double	Losing = 0;

	double holdingwinning = 0;//持仓盈亏
	double holdingposition = 0;
	double holdingprice = 0;
};

typedef std::map<std::string, UnitResult> Result;//key是合约 value是一个结果单位

class BacktestEngine :public QObject, public AlgoTradingAPI
{
	Q_OBJECT
signals :
	void sendMSG(const QString &msg);
	void setMaxProgressValue(int value);
	void setProgressValue(int value);
	void addStrategyItem(const QString &strategyname);
	void plotCapitalCurve(const PlotStruct &plotgod);
	void plotGodCurve(const PlotGodStruct &plot);
public:
	BacktestEngine();
	~BacktestEngine();

	void loadStrategy();

	std::vector<std::string> sendOrder(const std::string &symbol, const std::string &orderType, double price, double volume, StrategyTemplate* Strategy);
	void cancelOrder(const std::string &orderID, const std::string &gatewayName);
	void showLog(std::shared_ptr<Event>e);
	void writeAlgoTradingLog(const std::string &msg);
	void writeTradingReason(const std::shared_ptr<Event_Tick> &tick, const std::string &msg, StrategyTemplate *strategy);
	void writeTradingReason(const jsstructs::BarData &bar, const std::string &msg, StrategyTemplate *strategy);
	void PutEvent(std::shared_ptr<Event>e);
	std::vector<std::shared_ptr<Event_Tick>>loadTick(const std::string &tickDbName, const std::string &symbol, int days);
	std::vector<jsstructs::BarData>loadBar(const std::string &BarDbName, const std::string &symbol, int days);
	std::string getParam(const std::string &strategyname, const std::string &param);
private:
	EventEngine *eventengine;


	int orderID;
	int tradeCount;
	int progressbarValue;
	std::atomic<time_t> synchronous_datetime;
	std::atomic_int working_worker; 	std::mutex working_workermtx;	//同步锁
	std::condition_variable workingworker_cv;

	std::map<std::string, double>symbol_mapping_size;
	std::map<std::string, double>symbol_mapping_rate;
	std::map<std::string, double>symbol_mapping_slippage;

	std::map<std::string, std::shared_ptr<Event_Order>>allOrders;			std::mutex ordersmtx;	//cross orders
	std::map<std::string, std::shared_ptr<Event_Order>>workingOrders;
	std::map<std::string, std::map<std::string, std::vector<Event_Trade>>>holdingtradelist;/*strategy_mapping_symboltradelist*/					std::mutex holdingtradelistmtx;//open interest trade
	std::map<std::string, Result>backtestResultMap;							std::mutex backtestResultMapmtx;
	PlotGodStruct plotgodstruct;											std::mutex plotgodstructmtx;
	PlotStruct plotstruct;													std::mutex plotstructmtx;
	std::map<std::string, std::map<std::string, double>>DayMaxCapital;

	std::map<std::string, std::vector<StrategyTemplate*>>quotes_mapping_strategy;   std::mutex quotes_mapping_strategymtx;
	std::map<std::string, StrategyTemplate*>name_mapping_strategy;                  std::mutex name_mapping_strategymtx;
	std::map<std::string, StrategyTemplate*>orderID_mapping_strategy;				std::mutex orderID_mapping_strategymtx;
	std::map<std::string, HINSTANCE>dllMap;											//存放DLL
	
	time_t startDatetime;

	void clearStrategyObject();


	void readSymbolSize();
	void loadHistoryData(time_t startDateTime, time_t endDateTime, const std::string &symbol, std::vector<std::shared_ptr<Event_Tick>>&tick_vector);
	void loadHistoryData(time_t startDateTime, time_t endDateTime, const std::string &symbol, std::vector<jsstructs::BarData>&bar_vector);
	
	void processTickEvent(std::shared_ptr<Event>e);

	void processBarEvent(std::shared_ptr<Event>e);

	void crossLimitOrder(const std::shared_ptr<Event_Tick> &data);

	void crossLimitOrder(const jsstructs::BarData &data);

	void settlement(std::shared_ptr<Event_Trade>etrade);

	void recordPNL_Var(const jsstructs::BarData &data,const StrategyTemplate*strategy);

	void recordPNL_Var(const std::shared_ptr<Event_Tick> &data, const StrategyTemplate*strategy);

	void RecordCapital(const std::shared_ptr<Event_Tick> &data);

	void RecordCapital(const jsstructs::BarData &data);

	void savetraderecord(std::string strategyname, std::shared_ptr<Event_Trade>etrade);
	
	private slots:
	void runBacktest(time_t startDateTime, time_t endDateTime, const QString &TickorBar, const SymbolStrategyNameMap &symbol_mapping_strategyname);
};
#endif