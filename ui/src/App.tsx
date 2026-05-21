import { useState, useEffect, useRef } from 'react';
import './App.css';

interface Instrument {
  id: number;
  symbol: string;
  asset_class: string;
  exchange: string;
  saxo_uic: number;
  metadata?: any;
}

interface Alert {
  id: number;
  ts: string;
  screen: string;
  instrument_id: number;
  tier: string;
  payload: {
    symbol: string;
    price: number;
    trigger: string;
    [key: string]: any;
  };
  regime_at_alert: string;
  acted_on: number;
}

interface Regime {
  ts: string;
  regime: string;
  vix: number;
  breadth: number;
  hy_oas: number;
  spx_vs_200ma: number;
  detail?: any;
}

interface Candidate {
  id: number;
  created_ts: string;
  screen: string;
  instrument_id: number;
  entry_zone_low: number;
  entry_zone_high: number;
  suggested_stop: number;
  rr_target: number;
  notes: string;
  status: string;
}

function App() {
  const [activeTab, setActiveTab] = useState<'dashboard' | 'alerts' | 'candidates' | 'universe'>('dashboard');
  const [isConnected, setIsConnected] = useState<boolean>(false);
  const [instruments, setInstruments] = useState<Instrument[]>([]);
  const [alerts, setAlerts] = useState<Alert[]>([]);
  const [candidates, setCandidates] = useState<Candidate[]>([]);
  const [regime, setRegime] = useState<Regime | null>(null);
  const [ticks, setTicks] = useState<Record<string, number>>({});
  
  // Forms & Interactive State
  const [symbolSearch, setSymbolSearch] = useState<string>('');
  const [onboardAssetClass, setOnboardAssetClass] = useState<string>('Stock');
  const [onboardingStatus, setOnboardingStatus] = useState<string>('');
  const [toast, setToast] = useState<{ message: string; type: 'success' | 'error' | 'info' } | null>(null);

  // Candidates creation form
  const [showCandForm, setShowCandForm] = useState(false);
  const [candSymbol, setCandSymbol] = useState('');
  const [candScreen, setCandScreen] = useState('B');
  const [candLow, setCandLow] = useState('');
  const [candHigh, setCandHigh] = useState('');
  const [candStop, setCandStop] = useState('');
  const [candRR, setCandRR] = useState('3.0');
  const [candNotes, setCandNotes] = useState('');

  const wsRef = useRef<WebSocket | null>(null);

  // Fetch initial data
  const fetchData = async () => {
    try {
      const [resRegime, resAlerts, resCandidates, resInstruments] = await Promise.all([
        fetch('/api/regime'),
        fetch('/api/alerts'),
        fetch('/api/candidates'),
        fetch('/api/instruments')
      ]);

      if (resRegime.ok) {
        const data = await resRegime.json();
        if (data.length > 0) setRegime(data[0]);
      }
      if (resAlerts.ok) setAlerts(await resAlerts.json());
      if (resCandidates.ok) setCandidates(await resCandidates.json());
      if (resInstruments.ok) setInstruments(await resInstruments.json());
    } catch (e) {
      console.error('Failed to fetch screener data:', e);
      showToast('Error syncing with engine. Retrying...', 'error');
    }
  };

  useEffect(() => {
    // Add dark mode class globally to match neon-noir styles
    document.documentElement.classList.add('dark');
    fetchData();

    // Setup WebSocket
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws/live`;
    
    const connectWs = () => {
      console.log('Connecting to WebSocket:', wsUrl);
      const ws = new WebSocket(wsUrl);
      wsRef.current = ws;

      ws.onopen = () => {
        setIsConnected(true);
        showToast('Connected to live trading engine feed', 'success');
      };

      ws.onclose = () => {
        setIsConnected(false);
        setTimeout(connectWs, 3000); // Reconnect loop
      };

      ws.onmessage = (event) => {
        try {
          const message = JSON.parse(event.data);
          if (message.type === 'tick') {
            const tick = message.data;
            setTicks((prev) => ({ ...prev, [tick.symbol]: tick.last }));
          } else if (message.type === 'alert') {
            const alert = message.data;
            setAlerts((prev) => [alert, ...prev]);
            showToast(`🚨 ALERT TRIGGERED: ${alert.screen} on ${alert.payload.symbol} at ${alert.payload.price}`, 'info');
          } else if (message.type === 'regime') {
            setRegime(message.data);
            showToast(`Market Regime shifted: ${message.data.regime.toUpperCase()}`, 'info');
          }
        } catch (e) {
          console.error('WebSocket parse error:', e);
        }
      };
    };

    connectWs();

    return () => {
      if (wsRef.current) wsRef.current.close();
    };
  }, []);

  const showToast = (message: string, type: 'success' | 'error' | 'info') => {
    setToast({ message, type });
    setTimeout(() => setToast(null), 5000);
  };

  // Onboard new instrument
  const handleOnboardSymbol = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!symbolSearch) return;
    setOnboardingStatus('Searching Saxo Bank...');

    try {
      const response = await fetch('/api/instruments', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ symbol: symbolSearch.toUpperCase(), asset_class: onboardAssetClass })
      });

      const data = await response.json();
      if (response.ok) {
        showToast(`Successfully onboarded ${data.symbol} (UIC: ${data.saxo_uic})`, 'success');
        setSymbolSearch('');
        setOnboardingStatus('');
        fetchData();
      } else {
        showToast(data.error || 'Failed to onboard symbol', 'error');
        setOnboardingStatus('');
      }
    } catch (err) {
      showToast('Connection error onboarding symbol', 'error');
      setOnboardingStatus('');
    }
  };

  // Place Order/Act on alert
  const handleActOnAlert = async (alertId: number, action: 'execute' | 'dismiss') => {
    try {
      const response = await fetch('/api/alert_response', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ alert_id: alertId, action })
      });
      const data = await response.json();
      if (response.ok) {
        if (action === 'execute') {
          showToast(data.message || 'Trade executed successfully!', 'success');
        } else {
          showToast('Alert dismissed.', 'info');
        }
        fetchData();
      } else {
        showToast(data.error || 'Failed to act on alert', 'error');
      }
    } catch (e) {
      showToast('Network error handling alert', 'error');
    }
  };

  // Create new trading setup candidate
  const handleCreateCandidate = async (e: React.FormEvent) => {
    e.preventDefault();
    const inst = instruments.find(i => i.symbol === candSymbol.toUpperCase());
    if (!inst) {
      showToast(`Symbol ${candSymbol} not onboarded in database yet.`, 'error');
      return;
    }

    try {
      const response = await fetch('/api/candidates', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          symbol: candSymbol.toUpperCase(),
          screen: candScreen,
          entry_zone_low: candLow,
          entry_zone_high: candHigh,
          suggested_stop: candStop,
          rr_target: candRR,
          notes: candNotes
        })
      });

      const data = await response.json();
      if (response.ok) {
        showToast(`Setup Candidate created for ${candSymbol.toUpperCase()}`, 'success');
        setShowCandForm(false);
        // Clear fields
        setCandSymbol('');
        setCandLow('');
        setCandHigh('');
        setCandStop('');
        setCandRR('3.0');
        setCandNotes('');
        fetchData();
      } else {
        showToast(data.error || 'Failed to create candidate', 'error');
      }
    } catch (err) {
      showToast('Network error creating candidate', 'error');
    }
  };

  const getRegimeColor = (state: string) => {
    switch (state?.toLowerCase()) {
      case 'bull': return 'border-emerald-500/30 text-emerald-400 bg-emerald-500/5 glow-emerald';
      case 'chop': return 'border-amber-500/30 text-amber-400 bg-amber-500/5 glow-amber';
      case 'stress': return 'border-orange-500/30 text-orange-400 bg-orange-500/5 glow-orange';
      case 'crisis': return 'border-red-500/30 text-red-400 bg-red-500/5 glow-red';
      default: return 'border-gray-500/30 text-gray-400 bg-gray-500/5';
    }
  };

  return (
    <div className="min-h-screen bg-[#0b0c10] text-[#e0e1e6] flex flex-col font-sans select-none antialiased">
      
      {/* Toast Notification Banner */}
      {toast && (
        <div className={`fixed top-4 right-4 z-50 px-6 py-3 rounded-lg border shadow-xl flex items-center gap-3 animate-slide-in backdrop-blur-md ${
          toast.type === 'success' ? 'bg-emerald-950/80 border-emerald-500/30 text-emerald-300' :
          toast.type === 'error' ? 'bg-red-950/80 border-red-500/30 text-red-300' :
          'bg-[#1a1c26]/90 border-[#2f3142] text-[#ff6d5a] shadow-glow-primary'
        }`}>
          <span className="font-semibold text-sm">{toast.message}</span>
        </div>
      )}

      {/* Header Bar */}
      <header className="glass border-b border-white/5 py-4 px-6 flex items-center justify-between sticky top-0 z-40">
        <div className="flex items-center gap-3">
          <div className="w-8 h-8 rounded bg-gradient-to-tr from-[#ff6d5a] to-[#825aff] flex items-center justify-center font-bold text-black text-sm">
            T
          </div>
          <div>
            <h1 className="text-xl font-bold tracking-tight m-0 text-white leading-none">TACHYON</h1>
            <p className="text-[10px] text-gray-400 mt-1 uppercase tracking-wider">C++ Multi-Screen Engine</p>
          </div>
        </div>

        {/* Navigation Tabs */}
        <nav className="flex gap-1 bg-black/40 p-1 rounded-lg border border-white/5">
          {(['dashboard', 'alerts', 'candidates', 'universe'] as const).map((tab) => (
            <button
              key={tab}
              onClick={() => setActiveTab(tab)}
              className={`px-4 py-2 rounded-md text-xs font-semibold uppercase tracking-wider transition-all duration-200 ${
                activeTab === tab 
                  ? 'bg-gradient-to-r from-[#ff6d5a] to-[#ff8c7a] text-black shadow-md font-bold' 
                  : 'text-gray-400 hover:text-white hover:bg-white/5'
              }`}
            >
              {tab}
            </button>
          ))}
        </nav>

        {/* Engine Status */}
        <div className="flex items-center gap-4 text-xs">
          <div className="flex items-center gap-2">
            <span className={`w-2.5 h-2.5 rounded-full ${isConnected ? 'bg-emerald-500 animate-pulse' : 'bg-red-500'}`}></span>
            <span className="text-gray-400">Stream: {isConnected ? 'Live' : 'Offline'}</span>
          </div>
          <div className="text-gray-500 border-l border-white/10 pl-4">
            Port: <span className="text-gray-300">8080</span>
          </div>
        </div>
      </header>

      {/* Main Workspace Body */}
      <main className="flex-1 p-8 max-w-7xl mx-auto w-full">
        {activeTab === 'dashboard' && (
          <div className="space-y-8 animate-fade-in">
            {/* Market Regime & Key Indicators Grid */}
            <section className="grid grid-cols-1 md:grid-cols-4 gap-6">
              
              {/* Regime Widget */}
              <div className={`glass border p-6 rounded-2xl flex flex-col justify-between col-span-2 ${getRegimeColor(regime?.regime || 'chop')}`}>
                <div>
                  <span className="text-[10px] uppercase font-bold tracking-widest text-gray-400">Current Market Regime</span>
                  <h2 className="text-4xl font-extrabold uppercase mt-2 tracking-tight">
                    {regime?.regime || 'LOADING...'}
                  </h2>
                  <p className="text-xs text-gray-400 mt-2 font-medium">
                    Classifier monitors high-yield credit, index momentum, and implied volatility.
                  </p>
                </div>
                <div className="mt-8 flex justify-between items-end">
                  <div className="text-xs text-gray-500">
                    Shifted: <span className="text-gray-300 font-mono">{regime?.ts ? new Date(regime.ts).toLocaleTimeString() : 'N/A'}</span>
                  </div>
                  <div className="px-3 py-1 rounded bg-black/40 text-[10px] font-bold tracking-widest uppercase">
                    SYS ENABLED
                  </div>
                </div>
              </div>

              {/* VIX Widget */}
              <div className="glass border border-white/5 p-6 rounded-2xl flex flex-col justify-between">
                <div>
                  <span className="text-[10px] uppercase font-bold tracking-widest text-gray-400">Implied Volatility (VIX)</span>
                  <div className="text-3xl font-bold mt-2 text-white font-mono">
                    {regime?.vix ? regime.vix.toFixed(2) : '--.--'}
                  </div>
                  <div className="w-full bg-white/5 rounded-full h-1.5 mt-4 overflow-hidden">
                    <div 
                      className="bg-orange-500 h-full transition-all duration-500" 
                      style={{ width: `${Math.min(((regime?.vix || 15) / 40) * 100, 100)}%` }}
                    ></div>
                  </div>
                </div>
                <span className="text-[10px] text-gray-500 mt-4">
                  Low Regime Stress (&lt; 20)
                </span>
              </div>

              {/* Breadth Widget */}
              <div className="glass border border-white/5 p-6 rounded-2xl flex flex-col justify-between">
                <div>
                  <span className="text-[10px] uppercase font-bold tracking-widest text-gray-400">Market Breadth</span>
                  <div className="text-3xl font-bold mt-2 text-[#825aff] font-mono">
                    {regime?.breadth ? `${(regime.breadth * 100).toFixed(0)}%` : '--%'}
                  </div>
                  <div className="w-full bg-white/5 rounded-full h-1.5 mt-4 overflow-hidden">
                    <div 
                      className="bg-[#825aff] h-full transition-all duration-500" 
                      style={{ width: `${(regime?.breadth || 0.5) * 100}%` }}
                    ></div>
                  </div>
                </div>
                <span className="text-[10px] text-gray-500 mt-4">
                  % of stocks above 50-day SMA
                </span>
              </div>
            </section>

            {/* Live Dashboard Feeds */}
            <section className="grid grid-cols-1 lg:grid-cols-3 gap-8">
              
              {/* Left Column: Recent Signals */}
              <div className="lg:col-span-2 space-y-6">
                <div className="flex justify-between items-center">
                  <h3 className="text-lg font-bold text-white tracking-tight">Recent Live Screen Signals</h3>
                  <button onClick={() => setActiveTab('alerts')} className="text-xs text-[#ff6d5a] hover:underline font-semibold">
                    View All Alerts ({alerts.length})
                  </button>
                </div>

                <div className="space-y-4">
                  {alerts.slice(0, 4).map((alert) => (
                    <div key={alert.id} className="glass border border-white/5 p-5 rounded-xl hover:border-white/10 transition duration-200">
                      <div className="flex justify-between items-start">
                        <div className="flex items-center gap-3">
                          <span className="px-2.5 py-1 rounded bg-[#ff6d5a]/10 border border-[#ff6d5a]/20 text-[#ff6d5a] font-mono text-xs font-bold">
                            Screen {alert.screen}
                          </span>
                          <span className="font-bold text-white text-base font-mono">
                            {alert.payload.symbol}
                          </span>
                        </div>
                        <div className="flex items-center gap-2">
                          <span className={`text-[10px] px-2 py-0.5 rounded font-bold uppercase tracking-wider ${
                            alert.tier === 'premium' ? 'bg-[#ff6d5a]/20 text-[#ff6d5a]' : 'bg-[#825aff]/20 text-[#825aff]'
                          }`}>
                            {alert.tier}
                          </span>
                          <span className="text-[10px] text-gray-500 font-mono">
                            {new Date(alert.ts).toLocaleTimeString()}
                          </span>
                        </div>
                      </div>
                      
                      <p className="text-xs text-gray-300 mt-3 font-medium">
                        {alert.payload.trigger}
                      </p>

                      <div className="mt-4 pt-4 border-t border-white/5 flex justify-between items-center">
                        <div className="text-xs font-mono text-gray-400">
                          Price At Alert: <span className="text-white font-bold">${alert.payload.price}</span>
                        </div>

                        {alert.acted_on === 0 ? (
                          <div className="flex gap-2">
                            <button 
                              onClick={() => handleActOnAlert(alert.id, 'dismiss')}
                              className="px-3 py-1.5 rounded bg-white/5 hover:bg-white/10 text-gray-300 text-xs font-medium transition"
                            >
                              Dismiss
                            </button>
                            <button 
                              onClick={() => handleActOnAlert(alert.id, 'execute')}
                              className="px-3 py-1.5 rounded bg-[#ff6d5a] hover:bg-[#ff8c7a] text-black text-xs font-bold transition shadow-lg shadow-[#ff6d5a]/10"
                            >
                              Act / Place Order
                            </button>
                          </div>
                        ) : (
                          <span className="text-xs text-emerald-400 font-semibold flex items-center gap-1.5">
                            ✓ Acted & Filled
                          </span>
                        )}
                      </div>
                    </div>
                  ))}
                  {alerts.length === 0 && (
                    <div className="glass border border-white/5 p-8 rounded-xl text-center text-gray-500 text-xs">
                      No active alerts generated by the engine yet. Ticks will trigger screens.
                    </div>
                  )}
                </div>
              </div>

              {/* Right Column: Mini Watchlist / Onboard widget */}
              <div className="space-y-6">
                <h3 className="text-lg font-bold text-white tracking-tight">Onboard New Assets</h3>
                
                <div className="glass border border-white/5 p-6 rounded-xl space-y-4">
                  <p className="text-xs text-gray-400 leading-relaxed">
                    Search and query Saxo Bank OpenAPI to register new stocks or ETFs into the sytem universe.
                  </p>
                  
                  <form onSubmit={handleOnboardSymbol} className="space-y-3">
                    <input 
                      type="text" 
                      placeholder="Symbol (e.g. TSLA, NVDA)" 
                      value={symbolSearch}
                      onChange={(e) => setSymbolSearch(e.target.value)}
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-4 py-2.5 text-xs text-white placeholder-gray-500 focus:outline-none focus:border-[#ff6d5a]"
                    />
                    <div className="grid grid-cols-2 gap-2">
                      <select 
                        value={onboardAssetClass} 
                        onChange={(e) => setOnboardAssetClass(e.target.value)}
                        className="bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-gray-300 focus:outline-none"
                      >
                        <option value="Stock">Stock</option>
                        <option value="ETF">ETF</option>
                      </select>
                      <button 
                        type="submit"
                        className="bg-gradient-to-r from-[#ff6d5a] to-[#ff8c7a] text-black text-xs font-bold rounded-lg py-2 hover:opacity-90 transition"
                      >
                        Onboard
                      </button>
                    </div>
                  </form>
                  {onboardingStatus && (
                    <div className="text-[10px] text-amber-400 animate-pulse font-medium">
                      {onboardingStatus}
                    </div>
                  )}
                </div>

                {/* Real-time Ticks Widget */}
                <h3 className="text-lg font-bold text-white tracking-tight pt-2">Live Price Stream</h3>
                <div className="glass border border-white/5 rounded-xl divide-y divide-white/5 overflow-hidden">
                  {instruments.slice(0, 5).map((inst) => {
                    const price = ticks[inst.symbol];
                    return (
                      <div key={inst.id} className="p-4 flex justify-between items-center hover:bg-white/5 transition">
                        <div>
                          <div className="font-bold text-xs text-white font-mono">{inst.symbol}</div>
                          <div className="text-[9px] text-gray-500 uppercase">{inst.asset_class}</div>
                        </div>
                        <div className="text-right">
                          <div className="font-bold text-xs font-mono text-emerald-400">
                            {price ? `$${price.toFixed(2)}` : 'Wait...'}
                          </div>
                          <span className="text-[9px] text-gray-500 font-mono">Live L1 Quote</span>
                        </div>
                      </div>
                    );
                  })}
                </div>
              </div>
            </section>
          </div>
        )}

        {/* Alerts Page Tab */}
        {activeTab === 'alerts' && (
          <div className="space-y-6 animate-fade-in">
            <div className="flex justify-between items-center">
              <div>
                <h2 className="text-2xl font-bold text-white">Market Screener Alerts</h2>
                <p className="text-xs text-gray-400 mt-1">Real-time alerts generated across active algorithmic screens.</p>
              </div>
              <div className="text-xs text-gray-400">
                Total Signals: <span className="text-white font-bold font-mono">{alerts.length}</span>
              </div>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
              {alerts.map((alert) => (
                <div key={alert.id} className="glass border border-white/5 p-6 rounded-xl hover:border-white/10 transition">
                  <div className="flex justify-between items-start">
                    <div className="flex items-center gap-3">
                      <span className="px-2.5 py-1 rounded bg-[#ff6d5a]/10 border border-[#ff6d5a]/20 text-[#ff6d5a] font-mono text-xs font-bold">
                        Screen {alert.screen}
                      </span>
                      <span className="font-bold text-white text-base font-mono">
                        {alert.payload.symbol}
                      </span>
                    </div>
                    <span className="text-[10px] text-gray-500 font-mono">
                      {new Date(alert.ts).toLocaleString()}
                    </span>
                  </div>

                  <p className="text-xs text-gray-300 mt-4 leading-relaxed">
                    {alert.payload.trigger}
                  </p>

                  {/* Render technical detail points */}
                  <div className="grid grid-cols-2 gap-3 mt-4 bg-black/20 p-3 rounded-lg border border-white/5">
                    {Object.entries(alert.payload)
                      .filter(([key]) => key !== 'symbol' && key !== 'trigger' && key !== 'price')
                      .map(([key, val]) => (
                        <div key={key} className="text-xs flex justify-between">
                          <span className="text-gray-500 capitalize">{key.replace('_', ' ')}:</span>
                          <span className="text-gray-300 font-mono font-medium">{val}</span>
                        </div>
                    ))}
                  </div>

                  <div className="mt-5 pt-4 border-t border-white/5 flex justify-between items-center">
                    <div className="text-xs font-mono text-gray-400">
                      Execution Price: <span className="text-white font-bold">${alert.payload.price}</span>
                    </div>

                    {alert.acted_on === 0 ? (
                      <div className="flex gap-2">
                        <button 
                          onClick={() => handleActOnAlert(alert.id, 'dismiss')}
                          className="px-3 py-1.5 rounded bg-white/5 hover:bg-white/10 text-gray-300 text-xs font-medium transition"
                        >
                          Dismiss
                        </button>
                        <button 
                          onClick={() => handleActOnAlert(alert.id, 'execute')}
                          className="px-3 py-1.5 rounded bg-[#ff6d5a] hover:bg-[#ff8c7a] text-black text-xs font-bold transition"
                        >
                          Act / Place Order
                        </button>
                      </div>
                    ) : (
                      <span className="text-xs text-emerald-400 font-semibold flex items-center gap-1.5">
                        ✓ Acted & Filled
                      </span>
                    )}
                  </div>
                </div>
              ))}
              {alerts.length === 0 && (
                <div className="glass border border-white/5 p-12 rounded-xl text-center text-gray-500 text-sm col-span-2">
                  No alerts generated yet. Waiting for market price ticks...
                </div>
              )}
            </div>
          </div>
        )}

        {/* Candidates Page Tab */}
        {activeTab === 'candidates' && (
          <div className="space-y-6 animate-fade-in">
            <div className="flex justify-between items-center">
              <div>
                <h2 className="text-2xl font-bold text-white">Setup Candidates</h2>
                <p className="text-xs text-gray-400 mt-1">Pending and tracked positions awaiting confirmation entry zone.</p>
              </div>
              <button 
                onClick={() => setShowCandForm(!showCandForm)}
                className="bg-gradient-to-r from-[#ff6d5a] to-[#ff8c7a] text-black text-xs font-bold rounded-lg px-4 py-2 hover:opacity-90 transition"
              >
                {showCandForm ? 'Close Form' : 'New Setup Candidate'}
              </button>
            </div>

            {showCandForm && (
              <form onSubmit={handleCreateCandidate} className="glass border border-white/5 p-6 rounded-xl max-w-xl space-y-4">
                <h3 className="font-bold text-sm text-white">Create Candidate Setup</h3>
                <div className="grid grid-cols-2 gap-4">
                  <div>
                    <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">Symbol</label>
                    <input 
                      type="text" 
                      value={candSymbol} 
                      onChange={(e) => setCandSymbol(e.target.value)} 
                      placeholder="e.g. SPY" 
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white"
                      required
                    />
                  </div>
                  <div>
                    <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">Screen Class</label>
                    <select 
                      value={candScreen} 
                      onChange={(e) => setCandScreen(e.target.value)}
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-gray-300"
                    >
                      <option value="A">Screen A - Mean Reversion</option>
                      <option value="B">Screen B - Pullback</option>
                      <option value="F">Screen F - Darvas Breakout</option>
                    </select>
                  </div>
                </div>

                <div className="grid grid-cols-4 gap-2">
                  <div>
                    <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">Entry Low</label>
                    <input 
                      type="number" 
                      step="any"
                      value={candLow} 
                      onChange={(e) => setCandLow(e.target.value)} 
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white"
                      required
                    />
                  </div>
                  <div>
                    <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">Entry High</label>
                    <input 
                      type="number" 
                      step="any"
                      value={candHigh} 
                      onChange={(e) => setCandHigh(e.target.value)} 
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white"
                      required
                    />
                  </div>
                  <div>
                    <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">Stop Loss</label>
                    <input 
                      type="number" 
                      step="any"
                      value={candStop} 
                      onChange={(e) => setCandStop(e.target.value)} 
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white"
                      required
                    />
                  </div>
                  <div>
                    <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">R:R Target</label>
                    <input 
                      type="text" 
                      value={candRR} 
                      onChange={(e) => setCandRR(e.target.value)} 
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white"
                      required
                    />
                  </div>
                </div>

                <div>
                  <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">Strategy Notes</label>
                  <textarea 
                    value={candNotes} 
                    onChange={(e) => setCandNotes(e.target.value)} 
                    rows={2}
                    className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white"
                  ></textarea>
                </div>

                <button 
                  type="submit" 
                  className="bg-[#ff6d5a] hover:bg-[#ff8c7a] text-black text-xs font-bold rounded-lg px-4 py-2 w-full transition"
                >
                  Create Setup
                </button>
              </form>
            )}

            {/* Candidates Table */}
            <div className="glass border border-white/5 rounded-xl overflow-hidden">
              <table className="w-full text-left text-xs border-collapse">
                <thead>
                  <tr className="border-b border-white/5 bg-white/5 text-gray-400 font-bold uppercase tracking-wider">
                    <th className="p-4">Symbol</th>
                    <th className="p-4">Screen</th>
                    <th className="p-4">Entry Zone</th>
                    <th className="p-4">Stop Loss</th>
                    <th className="p-4">R:R Ratio</th>
                    <th className="p-4">Status</th>
                    <th className="p-4">Notes</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-white/5 font-mono text-gray-300">
                  {candidates.map((cand) => {
                    const inst = instruments.find(i => i.id === cand.instrument_id);
                    return (
                      <tr key={cand.id} className="hover:bg-white/5 transition">
                        <td className="p-4 font-bold text-white">{inst ? inst.symbol : 'UNKNOWN'}</td>
                        <td className="p-4">Screen {cand.screen}</td>
                        <td className="p-4 text-emerald-400">${cand.entry_zone_low} - ${cand.entry_zone_high}</td>
                        <td className="p-4 text-red-400">${cand.suggested_stop}</td>
                        <td className="p-4 text-sky-400">{cand.rr_target}:1</td>
                        <td className="p-4">
                          <span className={`px-2 py-0.5 rounded text-[10px] uppercase font-bold ${
                            cand.status === 'active' ? 'bg-emerald-500/10 text-emerald-400' :
                            cand.status === 'triggered' ? 'bg-[#ff6d5a]/15 text-[#ff6d5a]' :
                            'bg-gray-500/15 text-gray-400'
                          }`}>
                            {cand.status}
                          </span>
                        </td>
                        <td className="p-4 text-gray-400 font-sans font-medium">{cand.notes}</td>
                      </tr>
                    );
                  })}
                  {candidates.length === 0 && (
                    <tr>
                      <td colSpan={7} className="p-8 text-center text-gray-500 font-sans">
                        No active setup candidates registered in database.
                      </td>
                    </tr>
                  )}
                </tbody>
              </table>
            </div>
          </div>
        )}

        {/* Universe Tab */}
        {activeTab === 'universe' && (
          <div className="space-y-6 animate-fade-in">
            <div>
              <h2 className="text-2xl font-bold text-white">Screener Instrument Universe</h2>
              <p className="text-xs text-gray-400 mt-1">List of registered stocks and ETFs actively scanned by the C++ engine.</p>
            </div>

            <div className="glass border border-white/5 rounded-xl overflow-hidden">
              <table className="w-full text-left text-xs border-collapse">
                <thead>
                  <tr className="border-b border-white/5 bg-white/5 text-gray-400 font-bold uppercase tracking-wider">
                    <th className="p-4">ID</th>
                    <th className="p-4">Symbol</th>
                    <th className="p-4">Asset Class</th>
                    <th className="p-4">Exchange</th>
                    <th className="p-4">Saxo UIC</th>
                    <th className="p-4">Metadata</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-white/5 font-mono text-gray-300">
                  {instruments.map((inst) => (
                    <tr key={inst.id} className="hover:bg-white/5 transition">
                      <td className="p-4 text-gray-500">{inst.id}</td>
                      <td className="p-4 font-bold text-white">{inst.symbol}</td>
                      <td className="p-4">{inst.asset_class}</td>
                      <td className="p-4">{inst.exchange}</td>
                      <td className="p-4">{inst.saxo_uic}</td>
                      <td className="p-4 text-gray-400 font-sans text-[11px] truncate max-w-xs">
                        {inst.metadata ? JSON.stringify(inst.metadata) : '{}'}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
        )}
      </main>

      {/* Footer */}
      <footer className="glass border-t border-white/5 py-4 px-6 text-center text-xs text-gray-500">
        © 2026 Tachyon Ops. Built on high-performance C++ & SQLite3.
      </footer>
    </div>
  );
}

export default App;
