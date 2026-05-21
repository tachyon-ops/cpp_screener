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
  symbol: string;
  name: string;
  entry_zone_low: number;
  entry_zone_high: number;
  suggested_stop: number;
  rr_target: number;
  notes: string;
  status: string;
}

interface RotationResult {
  symbol: string;
  name: string;
  price: number;
  ma50: number;
  ma200: number;
  dist_50ma: number;
  dist_200ma: number;
  return_1m: number;
  return_3m: number;
  return_6m: number;
  return_12m: number;
  rs_rank: number;
  rs_percentile: number;
  cross_50_200: boolean;
  test_50ma: boolean;
  test_200ma: boolean;
}

function App() {
  const [activeTab, setActiveTab] = useState<'dashboard' | 'alerts' | 'candidates' | 'universe' | 'rotation' | 'settings'>('dashboard');
  const [isConnected, setIsConnected] = useState<boolean>(false);
  const [instruments, setInstruments] = useState<Instrument[]>([]);
  const [alerts, setAlerts] = useState<Alert[]>([]);
  const [candidates, setCandidates] = useState<Candidate[]>([]);
  const [regime, setRegime] = useState<Regime | null>(null);
  const [ticks, setTicks] = useState<Record<string, number>>({});
  const [candFilter, setCandFilter] = useState<'all' | 'active' | 'expired'>('active');
  const [settings, setSettings] = useState<Record<string, string>>({
    whatsapp_enabled: 'false',
    whatsapp_recipient: ''
  });
  
  // Sector Rotation State
  const [rotationData, setRotationData] = useState<RotationResult[]>([]);
  const [loadingRotation, setLoadingRotation] = useState<boolean>(false);
  const [rotationSortField, setRotationSortField] = useState<keyof RotationResult>('rs_rank');
  const [rotationSortAsc, setRotationSortAsc] = useState<boolean>(true);
  const [rotationFilter, setRotationFilter] = useState<string>('');
  const [viewMode, setViewMode] = useState<'grid' | 'table'>('grid');

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
      setLoadingRotation(true);
      const [resRegime, resAlerts, resCandidates, resInstruments, resRotation, resSettings] = await Promise.all([
        fetch('/api/regime'),
        fetch('/api/alerts'),
        fetch('/api/candidates'),
        fetch('/api/instruments'),
        fetch('/api/sector_rotation'),
        fetch('/api/settings')
      ]);

      if (resRegime.ok) {
        const data = await resRegime.json();
        if (data.length > 0) setRegime(data[0]);
      }
      if (resAlerts.ok) setAlerts(await resAlerts.json());
      if (resCandidates.ok) setCandidates(await resCandidates.json());
      if (resInstruments.ok) setInstruments(await resInstruments.json());
      if (resRotation.ok) setRotationData(await resRotation.json());
      if (resSettings.ok) {
        setSettings(await resSettings.json());
      }
    } catch (e) {
      console.error('Failed to fetch screener data:', e);
      showToast('Error syncing with engine. Retrying...', 'error');
    } finally {
      setLoadingRotation(false);
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

  // Sector Rotation Handlers
  const handleRecomputeRotation = async () => {
    try {
      showToast('Recomputing market regime & sector rotation board...', 'info');
      setLoadingRotation(true);
      const response = await fetch('/api/recompute', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({})
      });
      const data = await response.json();
      if (response.ok) {
        showToast('Recomputation completed successfully!', 'success');
        fetchData();
      } else {
        showToast(data.error || 'Failed to recompute', 'error');
        setLoadingRotation(false);
      }
    } catch (e) {
      showToast('Network error triggering recomputation', 'error');
      setLoadingRotation(false);
    }
  };

  const getPerformanceColor = (ret: number) => {
    if (ret >= 0.25) return 'border-emerald-500/40 text-emerald-400 bg-emerald-950/20 shadow-[0_0_12px_rgba(16,185,129,0.1)]';
    if (ret >= 0.10) return 'border-emerald-500/20 text-emerald-300 bg-emerald-950/10';
    if (ret > -0.10) return 'border-white/5 text-gray-300 bg-white/5';
    if (ret > -0.25) return 'border-rose-500/25 text-rose-300 bg-rose-950/10';
    return 'border-rose-500/40 text-rose-400 bg-rose-950/20 shadow-[0_0_12px_rgba(244,63,94,0.1)]';
  };

  const handleSort = (field: keyof RotationResult) => {
    if (rotationSortField === field) {
      setRotationSortAsc(!rotationSortAsc);
    } else {
      setRotationSortField(field);
      setRotationSortAsc(false); // default descending for sorting new fields
    }
  };

  const renderTableHeader = (field: keyof RotationResult, label: string) => {
    const isSorted = rotationSortField === field;
    return (
      <th 
        className="p-4 cursor-pointer hover:bg-white/5 select-none transition"
        onClick={() => handleSort(field)}
      >
        <div className="flex items-center gap-1">
          <span>{label}</span>
          {isSorted && (
            <span className="text-[10px] text-[#ff6d5a]">
              {rotationSortAsc ? '▲' : '▼'}
            </span>
          )}
        </div>
      </th>
    );
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

  const sortedRotation = [...rotationData]
    .filter(item => {
      const s = rotationFilter.toLowerCase();
      return item.symbol.toLowerCase().includes(s) || item.name.toLowerCase().includes(s);
    })
    .sort((a, b) => {
      const aVal = a[rotationSortField];
      const bVal = b[rotationSortField];
      
      if (typeof aVal === 'string' && typeof bVal === 'string') {
        const comp = aVal.localeCompare(bVal);
        return rotationSortAsc ? comp : -comp;
      }
      
      const aNum = typeof aVal === 'boolean' ? (aVal ? 1 : 0) : (aVal as number);
      const bNum = typeof bVal === 'boolean' ? (bVal ? 1 : 0) : (bVal as number);
      
      if (aNum < bNum) return rotationSortAsc ? -1 : 1;
      if (aNum > bNum) return rotationSortAsc ? 1 : -1;
      return 0;
    });

  const unactedAlertsCount = alerts.filter(a => a.acted_on === 0).length;

  return (
    <div className="h-screen w-screen bg-[#07080e] text-[#e0e1e6] flex overflow-hidden font-sans select-none antialiased">
      
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

      {/* Left Navigation Sidebar */}
      <aside className="w-64 border-r border-white/5 bg-[#090b11] flex flex-col justify-between h-full shrink-0">
        <div className="flex flex-col flex-1 overflow-y-auto">
          {/* Logo & Branding */}
          <div className="p-5 border-b border-white/5 flex items-center gap-3 bg-black/10">
            <div className="w-8 h-8 rounded bg-gradient-to-tr from-[#ff6d5a] to-[#825aff] flex items-center justify-center font-bold text-black text-sm shadow-[0_0_12px_rgba(255,109,90,0.3)]">
              T
            </div>
            <div>
              <h1 className="text-base font-extrabold tracking-tight m-0 text-white leading-none">TACHYON</h1>
              <p className="text-[9px] text-gray-500 mt-1.5 uppercase tracking-wider font-semibold">C++ Multi-Screen Engine</p>
            </div>
          </div>

          {/* Navigation Links */}
          <nav className="flex-1 px-3 py-4 space-y-1">
            {(['dashboard', 'alerts', 'candidates', 'universe', 'rotation', 'settings'] as const).map((tab) => {
              const tabMeta = {
                dashboard: { label: 'Dashboard', icon: '📊' },
                alerts: { label: 'Screener Alerts', icon: '🚨' },
                candidates: { label: 'Swing Watchlist', icon: '🎯' },
                universe: { label: 'Instrument Universe', icon: '🌌' },
                rotation: { label: 'Sector Heatmap', icon: '📈' },
                settings: { label: 'Settings', icon: '⚙️' }
              };
              const meta = tabMeta[tab];
              const isActive = activeTab === tab;
              return (
                <button
                  key={tab}
                  onClick={() => setActiveTab(tab)}
                  className={`w-full flex items-center justify-between px-3 py-2.5 rounded-lg text-xs font-semibold uppercase tracking-wider transition-all duration-150 ${
                    isActive 
                      ? 'bg-gradient-to-r from-[#ff6d5a]/15 to-transparent text-white border-l-2 border-[#ff6d5a]' 
                      : 'text-gray-400 hover:text-white hover:bg-white/5 border-l-2 border-transparent'
                  }`}
                >
                  <div className="flex items-center gap-2.5">
                    <span className="text-sm opacity-80">{meta.icon}</span>
                    <span>{meta.label}</span>
                  </div>
                  {tab === 'alerts' && unactedAlertsCount > 0 && (
                    <span className="px-2 py-0.5 rounded-full text-[9px] font-bold bg-[#ff6d5a] text-black animate-pulse font-mono">
                      {unactedAlertsCount}
                    </span>
                  )}
                </button>
              );
            })}
          </nav>
        </div>

        {/* Sidebar Footer: Engine Connection Status */}
        <div className="p-4 border-t border-white/5 bg-black/10 space-y-3">
          <div className="flex items-center justify-between text-xs">
            <div className="flex items-center gap-2">
              <span className={`w-2 h-2 rounded-full ${isConnected ? 'bg-emerald-500 animate-pulse' : 'bg-red-500'}`}></span>
              <span className="text-gray-400 font-medium">Stream: {isConnected ? 'Live' : 'Offline'}</span>
            </div>
            <span className="text-[10px] bg-white/5 border border-white/10 px-2 py-0.5 rounded text-gray-400 font-mono">
              Port 8080
            </span>
          </div>
        </div>
      </aside>

      {/* Main Workspace Frame */}
      <div className="flex-1 flex flex-col h-full overflow-hidden bg-[#0c0d14]">
        
        {/* Top bar header */}
        <header className="h-14 px-6 border-b border-white/5 flex items-center justify-between bg-[#090b11]/50 backdrop-blur-md">
            <span className="text-xs text-white font-bold uppercase tracking-widest bg-white/5 border border-white/5 px-2.5 py-1 rounded-md">
              {activeTab === 'rotation' ? 'Sector Heatmap' : activeTab === 'candidates' ? 'Swing Watchlist' : activeTab === 'universe' ? 'Instrument Universe' : activeTab === 'settings' ? 'Settings' : activeTab}
            </span>

          {/* Quick Regime status banner directly in header */}
          {regime && (
            <div className={`flex items-center gap-2 px-3 py-1 rounded-full border text-[10px] font-bold tracking-widest uppercase ${getRegimeColor(regime.regime)}`}>
              <span className="w-1.5 h-1.5 rounded-full bg-current animate-pulse"></span>
              Market Regime: {regime.regime}
            </div>
          )}
        </header>

        {/* Main Workspace Body */}
        <main className="flex-1 p-6 overflow-y-auto w-full">
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

            <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-4 gap-4">
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

        {/* Candidates Page Tab - Swing Watchlist Dashboard */}
        {activeTab === 'candidates' && (() => {
          const getCandidateTier = (notes: string) => {
            if (notes.includes('Tier: premium')) return 'premium';
            if (notes.includes('Tier: opportunity')) return 'opportunity';
            if (notes.includes('Tier: interesting')) return 'interesting';
            return 'manual';
          };

          const getTierBorderColor = (tier: string) => {
            switch (tier) {
              case 'premium': return 'border-rose-500/40 bg-rose-950/5 shadow-[0_0_15px_rgba(244,63,94,0.08)]';
              case 'opportunity': return 'border-[#825aff]/40 bg-[#825aff]/5 shadow-[0_0_15px_rgba(130,90,255,0.08)]';
              case 'interesting': return 'border-sky-500/30 bg-sky-950/5';
              default: return 'border-white/10 bg-white/5';
            }
          };

          const getTierBadgeColor = (tier: string) => {
            switch (tier) {
              case 'premium': return 'bg-rose-500/25 text-rose-300 border border-rose-500/35';
              case 'opportunity': return 'bg-[#825aff]/25 text-[#825aff] border border-[#825aff]/35';
              case 'interesting': return 'bg-sky-500/20 text-sky-400 border border-sky-500/25';
              default: return 'bg-gray-500/20 text-gray-400 border border-gray-500/20';
            }
          };

          const filteredCandidates = candidates.filter((cand) => {
            if (candFilter === 'active') return cand.status === 'active';
            if (candFilter === 'expired') return cand.status === 'expired' || cand.status === 'triggered';
            return true;
          });

          // Sort candidates: active first, premium tier first, then newest first
          const sortedCandidates = [...filteredCandidates].sort((a, b) => {
            if (a.status === 'active' && b.status !== 'active') return -1;
            if (a.status !== 'active' && b.status === 'active') return 1;

            const tierA = getCandidateTier(a.notes);
            const tierB = getCandidateTier(b.notes);
            const tierRank = { premium: 3, opportunity: 2, interesting: 1, manual: 0 };
            const rankDiff = tierRank[tierB] - tierRank[tierA];
            if (rankDiff !== 0) return rankDiff;

            return new Date(b.created_ts).getTime() - new Date(a.created_ts).getTime();
          });

          const activeCount = candidates.filter(c => c.status === 'active').length;
          const premiumCount = candidates.filter(c => getCandidateTier(c.notes) === 'premium' && c.status === 'active').length;
          const expiredCount = candidates.filter(c => c.status !== 'active').length;

          return (
            <div className="space-y-6 animate-fade-in">
              {/* Header Info */}
              <div className="flex flex-col md:flex-row justify-between items-start md:items-center gap-4">
                <div>
                  <h2 className="text-2xl font-bold text-white">Swing Pullback Watchlist</h2>
                  <p className="text-xs text-gray-400 mt-1">
                    Tracked setups that passed the Minervini Trend Template & volume contraction filters, currently monitoring entry levels.
                  </p>
                </div>
                <div className="flex gap-2">
                  <button
                    onClick={handleRecomputeRotation}
                    className="bg-white/5 hover:bg-white/10 text-white border border-white/10 text-xs font-semibold rounded-lg px-4 py-2 transition flex items-center gap-1.5"
                  >
                    <svg className="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M4 4v5h.582m15.356 2A8.001 8.001 0 1121.253 8H18" />
                    </svg>
                    Re-evaluate
                  </button>
                  <button 
                    onClick={() => setShowCandForm(!showCandForm)}
                    className="bg-[#ff6d5a] hover:bg-[#ff8c7a] text-black text-xs font-bold rounded-lg px-4 py-2 hover:opacity-90 transition flex items-center gap-1.5"
                  >
                    {showCandForm ? 'Close Form' : 'New Setup Candidate'}
                  </button>
                </div>
              </div>

              {/* Stats Bar */}
              <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
                <div className="glass border border-white/5 p-4 rounded-xl">
                  <div className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Active Watchlist</div>
                  <div className="text-2xl font-bold text-emerald-400 mt-1 font-mono">{activeCount}</div>
                  <div className="text-[9px] text-gray-500 mt-0.5">Monitoring buy zones</div>
                </div>
                <div className="glass border border-white/5 p-4 rounded-xl">
                  <div className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Premium Setups</div>
                  <div className="text-2xl font-bold text-rose-400 mt-1 font-mono">{premiumCount}</div>
                  <div className="text-[9px] text-gray-500 mt-0.5">Passing strict Minervini</div>
                </div>
                <div className="glass border border-white/5 p-4 rounded-xl">
                  <div className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Expired / Tripped</div>
                  <div className="text-2xl font-bold text-gray-400 mt-1 font-mono">{expiredCount}</div>
                  <div className="text-[9px] text-gray-500 mt-0.5">Auto-cleared positions</div>
                </div>
                <div className="glass border border-white/5 p-4 rounded-xl">
                  <div className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Avg. R:R Target</div>
                  <div className="text-2xl font-bold text-sky-400 mt-1 font-mono">
                    {candidates.length > 0
                      ? `${(candidates.reduce((acc, c) => acc + c.rr_target, 0) / candidates.length).toFixed(1)}:1`
                      : 'N/A'
                    }
                  </div>
                  <div className="text-[9px] text-gray-500 mt-0.5">Risk return profiles</div>
                </div>
              </div>

              {/* Filter controls and custom candidate forms */}
              <div className="flex flex-col sm:flex-row gap-3 justify-between items-center bg-black/35 p-3 rounded-xl border border-white/5">
                <div className="text-xs text-gray-400 font-semibold px-2">
                  Showing {sortedCandidates.length} watch items
                </div>

                <div className="flex bg-black/50 p-0.5 rounded-lg border border-white/10 w-full sm:w-auto">
                  {(['active', 'expired', 'all'] as const).map((filterOpt) => (
                    <button
                      key={filterOpt}
                      onClick={() => setCandFilter(filterOpt)}
                      className={`flex-1 sm:flex-none px-4 py-1.5 rounded-md text-[10px] uppercase font-bold tracking-wider transition ${
                        candFilter === filterOpt
                          ? 'bg-white/10 text-white shadow-sm'
                          : 'text-gray-400 hover:text-white'
                      }`}
                    >
                      {filterOpt} Setups
                    </button>
                  ))}
                </div>
              </div>

              {showCandForm && (
                <form onSubmit={handleCreateCandidate} className="glass border border-white/5 p-6 rounded-xl max-w-xl space-y-4">
                  <h3 className="font-bold text-sm text-white">Create Custom Watchlist Setup</h3>
                  <div className="grid grid-cols-2 gap-4">
                    <div>
                      <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">Symbol</label>
                      <input 
                        type="text" 
                        value={candSymbol} 
                        onChange={(e) => setCandSymbol(e.target.value)} 
                        placeholder="e.g. AAPL" 
                        className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white placeholder-gray-600 focus:outline-none focus:border-[#ff6d5a]"
                        required
                      />
                    </div>
                    <div>
                      <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">Screen Class</label>
                      <select 
                        value={candScreen} 
                        onChange={(e) => setCandScreen(e.target.value)}
                        className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-gray-300 focus:outline-none"
                      >
                        <option value="B">Screen B - Pullback</option>
                        <option value="A">Screen A - Mean Reversion</option>
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
                        className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white focus:outline-none"
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
                        className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white focus:outline-none"
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
                        className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white focus:outline-none"
                        required
                      />
                    </div>
                    <div>
                      <label className="block text-[10px] text-gray-400 uppercase font-bold mb-1">R:R Target</label>
                      <input 
                        type="text" 
                        value={candRR} 
                        onChange={(e) => setCandRR(e.target.value)} 
                        className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white focus:outline-none"
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
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2 text-xs text-white focus:outline-none"
                      placeholder="Enter setup observations..."
                    ></textarea>
                  </div>

                  <button 
                    type="submit" 
                    className="bg-[#ff6d5a] hover:bg-[#ff8c7a] text-black text-xs font-bold rounded-lg px-4 py-2.5 w-full transition"
                  >
                    Add to Watchlist
                  </button>
                </form>
              )}

              {/* Watchlist Cards Grid */}
              <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-6">
                {sortedCandidates.map((cand) => {
                  const symbol = cand.symbol || instruments.find(i => i.id === cand.instrument_id)?.symbol || 'UNKNOWN';
                  const name = cand.name || instruments.find(i => i.id === cand.instrument_id)?.metadata?.name || `${symbol} Common Stock`;
                  const price = ticks[symbol];
                  
                  const stopPrice = cand.suggested_stop;
                  const entryLow = cand.entry_zone_low;
                  const entryHigh = cand.entry_zone_high;
                  
                  const stopDiff = entryHigh - stopPrice;
                  const targetPrice = entryHigh + (cand.rr_target * stopDiff);

                  const tier = getCandidateTier(cand.notes);

                  // Setup indicators relative to live ticks
                  const inEntry = price !== undefined && price >= entryLow && price <= entryHigh;
                  const aboveEntry = price !== undefined && price > entryHigh;

                  // Percentages for the custom progress bar
                  const totalRange = targetPrice - stopPrice;
                  const entryLowPct = totalRange > 0 ? ((entryLow - stopPrice) / totalRange) * 100 : 25;
                  const entryHighPct = totalRange > 0 ? ((entryHigh - stopPrice) / totalRange) * 100 : 35;
                  
                  let pricePct = -1;
                  if (price !== undefined && totalRange > 0) {
                    pricePct = Math.max(0, Math.min(100, ((price - stopPrice) / totalRange) * 100));
                  }

                  return (
                    <div 
                      key={cand.id} 
                      className={`glass border p-5 rounded-2xl flex flex-col justify-between transition-all duration-300 hover:-translate-y-1 hover:shadow-lg relative overflow-hidden ${
                        cand.status === 'active' 
                          ? getTierBorderColor(tier) 
                          : 'border-white/5 bg-[#12131e]/50 opacity-60'
                      }`}
                    >
                      {/* Header with symbol, status, tier */}
                      <div>
                        <div className="flex justify-between items-start">
                          <div>
                            <div className="flex items-center gap-2">
                              <span className="font-mono font-extrabold text-xl text-white tracking-tight">{symbol}</span>
                              <span className={`px-2 py-0.5 rounded text-[8px] uppercase font-bold ${getTierBadgeColor(tier)}`}>
                                {tier}
                              </span>
                            </div>
                            <p className="text-[10px] text-gray-500 font-medium truncate max-w-[180px] mt-0.5" title={name}>
                              {name}
                            </p>
                          </div>

                          <div className="flex flex-col items-end">
                            <span className={`px-2 py-0.5 rounded text-[9px] uppercase font-bold font-mono tracking-wider ${
                              cand.status === 'active' ? 'bg-emerald-500/10 text-emerald-400' :
                              cand.status === 'triggered' ? 'bg-amber-500/10 text-amber-400' :
                              'bg-white/5 text-gray-400'
                            }`}>
                              {cand.status}
                            </span>
                            <span className="text-[8px] text-gray-500 font-mono mt-1">
                              {cand.created_ts.substr(11, 8) || '00:00:00'} UTC
                            </span>
                          </div>
                        </div>

                        {/* Strategy Info */}
                        <div className="mt-4 flex items-center justify-between">
                          <span className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Screen {cand.screen} Setup</span>
                          <span className="text-[10px] text-sky-400 font-mono font-bold">{cand.rr_target}:1 R:R Target</span>
                        </div>

                        {/* Live Price and Entry Status */}
                        <div className="mt-3 bg-black/30 rounded-xl p-3 border border-white/5 flex justify-between items-center">
                          <div className="flex flex-col">
                            <span className="text-[9px] text-gray-500 uppercase font-bold">Live Quote</span>
                            <span className="text-base font-bold font-mono text-white mt-0.5">
                              {price !== undefined ? `$${price.toFixed(2)}` : 'Awaiting Quote'}
                            </span>
                          </div>
                          
                          <div className="text-right">
                            <span className="text-[9px] text-gray-500 uppercase font-bold">Signal State</span>
                            <div className="mt-0.5">
                              {price === undefined ? (
                                <span className="text-[10px] text-gray-400 font-semibold font-mono">STANDBY</span>
                              ) : inEntry ? (
                                <span className="text-[10px] text-emerald-400 font-extrabold font-mono animate-pulse flex items-center gap-1">
                                  <span className="w-1.5 h-1.5 rounded-full bg-emerald-400"></span>
                                  IN BUY ZONE
                                </span>
                              ) : aboveEntry ? (
                                <span className="text-[10px] text-sky-400 font-bold font-mono">ABOVE ENTRY</span>
                              ) : (
                                <span className="text-[10px] text-amber-500 font-bold font-mono">BELOW ENTRY</span>
                              )}
                            </div>
                          </div>
                        </div>

                        {/* Interactive Visual Slider */}
                        <div className="mt-5 space-y-1.5">
                          <div className="relative h-1.5 bg-white/5 rounded-full overflow-visible">
                            {/* Stop Loss Dot */}
                            <div className="absolute left-0 -top-1 w-3.5 h-3.5 rounded-full bg-rose-600 border-2 border-[#0c0d14] shadow-md shadow-rose-900/30" title={`Stop: $${stopPrice.toFixed(2)}`}></div>
                            
                            {/* Entry Zone Range Highlight */}
                            <div 
                              className="absolute h-1.5 bg-emerald-500/25 border-x border-emerald-500/40"
                              style={{ left: `${entryLowPct}%`, width: `${entryHighPct - entryLowPct}%` }}
                            ></div>
                            
                            {/* Target Dot */}
                            <div className="absolute right-0 -top-1 w-3.5 h-3.5 rounded-full bg-sky-600 border-2 border-[#0c0d14] shadow-md shadow-sky-900/30" title={`Target: $${targetPrice.toFixed(2)}`}></div>
                            
                            {/* Current Price Pin */}
                            {pricePct >= 0 && (
                              <div 
                                className="absolute -top-1.5 -ml-2 transition-all duration-300 z-10 flex flex-col items-center"
                                style={{ left: `${pricePct}%` }}
                              >
                                <span className="w-4 h-4 rounded-full bg-white border-2 border-emerald-500 shadow-md animate-pulse"></span>
                              </div>
                            )}
                          </div>

                          <div className="flex justify-between text-[8px] font-mono text-gray-500 pt-0.5">
                            <span>Stop: ${stopPrice.toFixed(2)}</span>
                            <span className="text-emerald-400/80">Zone: ${entryLow.toFixed(2)}-${entryHigh.toFixed(2)}</span>
                            <span>Target: ${targetPrice.toFixed(2)}</span>
                          </div>
                        </div>
                      </div>

                      {/* Strategy Notes / Footer */}
                      <div className="mt-5 pt-3 border-t border-white/5">
                        <p className="text-[11px] text-gray-400 font-sans italic leading-relaxed">
                          {cand.notes.replace(/\(Tier:.*\)/, '').trim()}
                        </p>
                        <div className="flex justify-between items-center mt-3 text-[9px] text-gray-500">
                          <span>Created: {cand.created_ts.substr(0, 10)}</span>
                          <span className="font-mono">Setup ID: #{cand.id}</span>
                        </div>
                      </div>
                    </div>
                  );
                })}
                {sortedCandidates.length === 0 && (
                  <div className="glass border border-dashed border-white/10 p-12 rounded-2xl text-center text-gray-500 text-sm col-span-full">
                    No {candFilter !== 'all' ? candFilter : ''} swing watchlist items found.
                  </div>
                )}
              </div>
            </div>
          );
        })()}

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

        {/* Sector Rotation Tab */}
        {activeTab === 'rotation' && (
          <div className="space-y-6 animate-fade-in">
            {/* Page Title & Intro */}
            <div className="flex flex-col md:flex-row justify-between items-start md:items-center gap-4">
              <div>
                <h2 className="text-2xl font-bold text-white">Sector & Industry Rotation Heatmap</h2>
                <p className="text-xs text-gray-400 mt-1">
                  Relative Strength ranking and momentum metrics for major sector and industry ETFs.
                </p>
              </div>
              <div className="flex gap-2">
                <button
                  onClick={fetchData}
                  disabled={loadingRotation}
                  className="bg-white/5 hover:bg-white/10 text-white border border-white/10 text-xs font-semibold rounded-lg px-4 py-2 transition flex items-center gap-1 disabled:opacity-50"
                >
                  <svg className="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                    <path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M4 4v5h.582m15.356 2A8.001 8.001 0 1121.253 8H18" />
                  </svg>
                  Refresh Data
                </button>
                <button
                  onClick={handleRecomputeRotation}
                  disabled={loadingRotation}
                  className="bg-[#ff6d5a] hover:bg-[#ff8c7a] text-black text-xs font-bold rounded-lg px-4 py-2 transition flex items-center gap-1 disabled:opacity-50"
                >
                  {loadingRotation ? (
                    <span className="w-3.5 h-3.5 border-2 border-black border-t-transparent rounded-full animate-spin"></span>
                  ) : (
                    <svg className="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2M9 5a2 2 0 002 2h2a2 2 0 002-2M9 5a2 2 0 012-2h2a2 2 0 012 2" />
                    </svg>
                  )}
                  Recompute EOD
                </button>
              </div>
            </div>

            {/* Quick Summary Cards */}
            <div className="grid grid-cols-2 lg:grid-cols-4 gap-4">
              <div className="glass border border-white/5 p-4 rounded-xl">
                <div className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Total ETFs</div>
                <div className="text-2xl font-bold text-white mt-1 font-mono">{rotationData.length}</div>
                <div className="text-[9px] text-gray-500 mt-0.5">Actively tracked sectors</div>
              </div>
              <div className="glass border border-white/5 p-4 rounded-xl">
                <div className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Bullish Regime</div>
                <div className="text-2xl font-bold text-emerald-400 mt-1 font-mono">
                  {rotationData.filter(d => d.return_12m > 0).length}
                </div>
                <div className="text-[9px] text-gray-500 mt-0.5">12M return is positive</div>
              </div>
              <div className="glass border border-white/5 p-4 rounded-xl">
                <div className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Crossovers (5d)</div>
                <div className="text-2xl font-bold text-purple-400 mt-1 font-mono">
                  {rotationData.filter(d => d.cross_50_200).length}
                </div>
                <div className="text-[9px] text-gray-500 mt-0.5">Active Golden/Death crosses</div>
              </div>
              <div className="glass border border-white/5 p-4 rounded-xl">
                <div className="text-[10px] text-gray-400 font-bold uppercase tracking-wider">Boundary Tests</div>
                <div className="text-2xl font-bold text-cyan-400 mt-1 font-mono">
                  {rotationData.filter(d => d.test_50ma || d.test_200ma).length}
                </div>
                <div className="text-[9px] text-gray-500 mt-0.5">Price within 1% of MA</div>
              </div>
            </div>

            {/* Controls Bar */}
            <div className="flex flex-col sm:flex-row gap-3 justify-between items-center bg-black/35 p-3 rounded-xl border border-white/5">
              {/* Search */}
              <div className="relative w-full sm:w-80">
                <span className="absolute inset-y-0 left-0 pl-3 flex items-center pointer-events-none text-gray-500 text-xs">
                  🔍
                </span>
                <input
                  type="text"
                  placeholder="Filter by symbol or sector..."
                  value={rotationFilter}
                  onChange={(e) => setRotationFilter(e.target.value)}
                  className="w-full bg-black/40 border border-white/10 rounded-lg pl-9 pr-3 py-1.5 text-xs text-white placeholder-gray-500 focus:outline-none focus:border-white/20 transition font-sans"
                />
              </div>

              {/* View Toggle */}
              <div className="flex bg-black/50 p-0.5 rounded-lg border border-white/10 w-full sm:w-auto">
                <button
                  onClick={() => setViewMode('grid')}
                  className={`flex-1 sm:flex-none px-4 py-1.5 rounded-md text-[10px] uppercase font-bold tracking-wider transition ${
                    viewMode === 'grid'
                      ? 'bg-white/10 text-white shadow-sm'
                      : 'text-gray-400 hover:text-white'
                  }`}
                >
                  Grid Heatmap
                </button>
                <button
                  onClick={() => setViewMode('table')}
                  className={`flex-1 sm:flex-none px-4 py-1.5 rounded-md text-[10px] uppercase font-bold tracking-wider transition ${
                    viewMode === 'table'
                      ? 'bg-white/10 text-white shadow-sm'
                      : 'text-gray-400 hover:text-white'
                  }`}
                >
                  Details Table
                </button>
              </div>
            </div>

            {/* Loading Indicator */}
            {loadingRotation && rotationData.length === 0 ? (
              <div className="flex flex-col items-center justify-center p-12 space-y-4">
                <span className="w-8 h-8 border-4 border-[#ff6d5a] border-t-transparent rounded-full animate-spin"></span>
                <span className="text-xs text-gray-400">Loading sector rotation board...</span>
              </div>
            ) : sortedRotation.length === 0 ? (
              <div className="text-center py-12 text-gray-500 border border-dashed border-white/10 rounded-xl">
                No sector ETFs found matching search criteria.
              </div>
            ) : viewMode === 'grid' ? (
              /* Heatmap Grid View */
              <div className="grid grid-cols-1 sm:grid-cols-2 md:grid-cols-3 lg:grid-cols-4 xl:grid-cols-5 2xl:grid-cols-6 3xl:grid-cols-7 4xl:grid-cols-8 gap-4">
                {sortedRotation.map((item) => {
                  const price = ticks[item.symbol] || item.price;
                  return (
                    <div
                      key={item.symbol}
                      className={`glass border p-4 rounded-xl flex flex-col justify-between transition-all duration-300 hover:-translate-y-0.5 hover:shadow-lg ${getPerformanceColor(item.return_12m)}`}
                    >
                      <div>
                        <div className="flex justify-between items-start">
                          <div>
                            <span className="text-[10px] opacity-60 font-bold font-mono">#{item.rs_rank}</span>
                            <h3 className="text-lg font-bold tracking-tight text-white font-mono leading-tight">{item.symbol}</h3>
                          </div>
                          <span className="text-xs font-bold font-mono text-white/95">
                            ${price.toFixed(2)}
                          </span>
                        </div>
                        <p className="text-[10px] opacity-75 mt-1 truncate font-sans font-medium" title={item.name}>
                          {item.name}
                        </p>
                      </div>

                      {/* Performance Indicators */}
                      <div className="mt-4 grid grid-cols-2 gap-x-2 gap-y-1.5 border-t border-white/5 pt-3 font-mono text-[10px]">
                        <div className="flex justify-between">
                          <span className="opacity-50">1M:</span>
                          <span className={item.return_1m >= 0 ? 'text-emerald-400' : 'text-rose-400'}>
                            {item.return_1m >= 0 ? '+' : ''}{(item.return_1m * 100).toFixed(2)}%
                          </span>
                        </div>
                        <div className="flex justify-between">
                          <span className="opacity-50">3M:</span>
                          <span className={item.return_3m >= 0 ? 'text-emerald-400' : 'text-rose-400'}>
                            {item.return_3m >= 0 ? '+' : ''}{(item.return_3m * 100).toFixed(2)}%
                          </span>
                        </div>
                        <div className="flex justify-between">
                          <span className="opacity-50">6M:</span>
                          <span className={item.return_6m >= 0 ? 'text-emerald-400' : 'text-rose-400'}>
                            {item.return_6m >= 0 ? '+' : ''}{(item.return_6m * 100).toFixed(2)}%
                          </span>
                        </div>
                        <div className="flex justify-between">
                          <span className="opacity-50">12M:</span>
                          <span className={item.return_12m >= 0 ? 'text-emerald-400' : 'text-rose-400'}>
                            {item.return_12m >= 0 ? '+' : ''}{(item.return_12m * 100).toFixed(2)}%
                          </span>
                        </div>
                      </div>

                      {/* MA Distances & Badges */}
                      <div className="mt-3 border-t border-white/5 pt-3 space-y-2">
                        <div className="flex justify-between text-[9px] font-mono">
                          <div className="flex flex-col">
                            <span className="opacity-50">Dist 50MA</span>
                            <span className={`font-semibold ${item.dist_50ma >= 0 ? 'text-emerald-400' : 'text-rose-400'}`}>
                              {item.dist_50ma >= 0 ? '+' : ''}{(item.dist_50ma * 100).toFixed(1)}%
                            </span>
                          </div>
                          <div className="flex flex-col items-end">
                            <span className="opacity-50">Dist 200MA</span>
                            <span className={`font-semibold ${item.dist_200ma >= 0 ? 'text-emerald-400' : 'text-rose-400'}`}>
                              {item.dist_200ma >= 0 ? '+' : ''}{(item.dist_200ma * 100).toFixed(1)}%
                            </span>
                          </div>
                        </div>

                        {/* Event Badges */}
                        <div className="flex flex-wrap gap-1">
                          {item.cross_50_200 && (
                            <span className="px-1.5 py-0.5 rounded text-[8px] uppercase font-bold bg-purple-500/20 text-purple-300 border border-purple-500/20 animate-pulse">
                              MA Crossover
                            </span>
                          )}
                          {item.test_50ma && (
                            <span className="px-1.5 py-0.5 rounded text-[8px] uppercase font-bold bg-cyan-500/20 text-cyan-300 border border-cyan-500/20">
                              Test 50MA
                            </span>
                          )}
                          {item.test_200ma && (
                            <span className="px-1.5 py-0.5 rounded text-[8px] uppercase font-bold bg-indigo-500/20 text-indigo-300 border border-indigo-500/20">
                              Test 200MA
                            </span>
                          )}
                        </div>
                      </div>
                    </div>
                  );
                })}
              </div>
            ) : (
              /* Table View */
              <div className="glass border border-white/5 rounded-xl overflow-hidden">
                <table className="w-full text-left text-xs border-collapse">
                  <thead>
                    <tr className="border-b border-white/5 bg-white/5 text-gray-400 font-bold uppercase tracking-wider">
                      {renderTableHeader('rs_rank', 'Rank')}
                      {renderTableHeader('symbol', 'Symbol')}
                      {renderTableHeader('name', 'Name')}
                      {renderTableHeader('price', 'Price')}
                      {renderTableHeader('return_1m', '1M')}
                      {renderTableHeader('return_3m', '3M')}
                      {renderTableHeader('return_6m', '6M')}
                      {renderTableHeader('return_12m', '12M')}
                      {renderTableHeader('dist_50ma', 'd/50MA')}
                      {renderTableHeader('dist_200ma', 'd/200MA')}
                      <th className="p-4">Status</th>
                    </tr>
                  </thead>
                  <tbody className="divide-y divide-white/5 font-mono text-gray-300">
                    {sortedRotation.map((item) => {
                      const price = ticks[item.symbol] || item.price;
                      return (
                        <tr key={item.symbol} className="hover:bg-white/5 transition">
                          <td className="p-4 font-bold text-white">#{item.rs_rank}</td>
                          <td className="p-4 font-bold text-white text-sm">{item.symbol}</td>
                          <td className="p-4 text-gray-400 font-sans font-medium">{item.name}</td>
                          <td className="p-4 font-bold text-white">${price.toFixed(2)}</td>
                          <td className={`p-4 ${item.return_1m >= 0 ? 'text-emerald-400' : 'text-rose-400'}`}>
                            {item.return_1m >= 0 ? '+' : ''}{(item.return_1m * 100).toFixed(2)}%
                          </td>
                          <td className={`p-4 ${item.return_3m >= 0 ? 'text-emerald-400' : 'text-rose-400'}`}>
                            {item.return_3m >= 0 ? '+' : ''}{(item.return_3m * 100).toFixed(2)}%
                          </td>
                          <td className={`p-4 ${item.return_6m >= 0 ? 'text-emerald-400' : 'text-rose-400'}`}>
                            {item.return_6m >= 0 ? '+' : ''}{(item.return_6m * 100).toFixed(2)}%
                          </td>
                          <td className={`p-4 ${item.return_12m >= 0 ? 'text-emerald-400' : 'text-rose-400'}`}>
                            {item.return_12m >= 0 ? '+' : ''}{(item.return_12m * 100).toFixed(2)}%
                          </td>
                          <td className={`p-4 ${item.dist_50ma >= 0 ? 'text-emerald-400' : 'text-rose-400'}`}>
                            {item.dist_50ma >= 0 ? '+' : ''}{(item.dist_50ma * 100).toFixed(1)}%
                          </td>
                          <td className={`p-4 ${item.dist_200ma >= 0 ? 'text-emerald-400' : 'text-rose-400'}`}>
                            {item.dist_200ma >= 0 ? '+' : ''}{(item.dist_200ma * 100).toFixed(1)}%
                          </td>
                          <td className="p-4">
                            <div className="flex gap-1">
                              {item.cross_50_200 && (
                                <span className="px-1.5 py-0.5 rounded text-[8px] uppercase font-bold bg-purple-500/20 text-purple-300">
                                  Cross
                                </span>
                              )}
                              {item.test_50ma && (
                                <span className="px-1.5 py-0.5 rounded text-[8px] uppercase font-bold bg-cyan-500/20 text-cyan-300">
                                  T/50
                                </span>
                              )}
                              {item.test_200ma && (
                                <span className="px-1.5 py-0.5 rounded text-[8px] uppercase font-bold bg-indigo-500/20 text-indigo-300">
                                  T/200
                                </span>
                              )}
                            </div>
                          </td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            )}
          </div>
        )}

        {/* Settings Tab */}
        {activeTab === 'settings' && (
          <div className="space-y-6 max-w-4xl animate-fade-in">
            <div>
              <h2 className="text-2xl font-bold text-white">System Settings</h2>
              <p className="text-xs text-gray-400 mt-1">
                Configure trading system parameters, API connections, and real-time notification endpoints.
              </p>
            </div>

            <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
              {/* WhatsApp Bot Configuration Card */}
              <div className="glass border border-white/5 p-6 rounded-xl space-y-6 bg-black/20">
                <div className="flex items-center gap-3">
                  <div className="w-10 h-10 rounded-lg bg-emerald-500/10 border border-emerald-500/20 flex items-center justify-center text-lg text-emerald-400 shadow-[0_0_15px_rgba(16,185,129,0.1)]">
                    💬
                  </div>
                  <div>
                    <h3 className="font-bold text-sm text-white">WhatsApp Notifications</h3>
                    <p className="text-[10px] text-gray-400">Configure BuilderBot alerts</p>
                  </div>
                </div>

                <div className="space-y-4">
                  {/* Toggle */}
                  <div className="flex items-center justify-between p-3.5 bg-black/40 border border-white/5 rounded-lg">
                    <div>
                      <div className="text-xs font-bold text-white uppercase tracking-wider">Enable WhatsApp Alerts</div>
                      <div className="text-[10px] text-gray-500 mt-0.5">Route real-time screener signals via WhatsApp</div>
                    </div>
                    <button
                      onClick={() => setSettings(prev => ({ ...prev, whatsapp_enabled: prev.whatsapp_enabled === 'true' ? 'false' : 'true' }))}
                      className={`relative inline-flex h-5 w-10 shrink-0 cursor-pointer rounded-full border-2 border-transparent transition-colors duration-200 ease-in-out focus:outline-none ${
                        settings.whatsapp_enabled === 'true' ? 'bg-[#ff6d5a]' : 'bg-white/10'
                      }`}
                    >
                      <span
                        className={`pointer-events-none inline-block h-4 w-4 transform rounded-full bg-white shadow ring-0 transition duration-200 ease-in-out ${
                          settings.whatsapp_enabled === 'true' ? 'translate-x-5' : 'translate-x-0'
                        }`}
                      />
                    </button>
                  </div>

                  {/* Recipient Input */}
                  <div className="space-y-1.5">
                    <label className="block text-[10px] text-gray-400 uppercase font-bold">Recipient Phone Number</label>
                    <input
                      type="text"
                      placeholder="e.g. 351912345678"
                      value={settings.whatsapp_recipient || ''}
                      onChange={(e) => setSettings(prev => ({ ...prev, whatsapp_recipient: e.target.value }))}
                      className="w-full bg-black/40 border border-white/10 rounded-lg px-3 py-2.5 text-xs text-white placeholder-gray-600 focus:outline-none focus:border-[#ff6d5a] font-mono"
                    />
                    <p className="text-[9px] text-gray-500">Include country code without + or spaces (e.g. 14155552671)</p>
                  </div>
                </div>
              </div>

              {/* Engine System Stats & Help Card */}
              <div className="glass border border-white/5 p-6 rounded-xl space-y-6 bg-black/20 flex flex-col justify-between">
                <div className="space-y-4">
                  <div className="flex items-center gap-3">
                    <div className="w-10 h-10 rounded-lg bg-[#825aff]/10 border border-[#825aff]/20 flex items-center justify-center text-lg text-[#825aff] shadow-[0_0_15px_rgba(130,90,255,0.1)]">
                      🤖
                    </div>
                    <div>
                      <h3 className="font-bold text-sm text-white">WhatsApp Bot Commands</h3>
                      <p className="text-[10px] text-gray-400">Control the engine remotely on WhatsApp</p>
                    </div>
                  </div>

                  <div className="bg-black/30 border border-white/5 rounded-lg p-4 space-y-2.5 font-mono text-[10px]">
                    <div className="flex justify-between text-gray-400 border-b border-white/5 pb-1">
                      <span>Command</span>
                      <span>Description</span>
                    </div>
                    <div className="flex justify-between">
                      <span className="text-[#ff6d5a]">/regime</span>
                      <span className="text-gray-400 text-right">Get current market regime</span>
                    </div>
                    <div className="flex justify-between">
                      <span className="text-[#ff6d5a]">/candidates</span>
                      <span className="text-gray-400 text-right">List active swing setups</span>
                    </div>
                    <div className="flex justify-between">
                      <span className="text-[#ff6d5a]">/alerts</span>
                      <span className="text-gray-400 text-right">List recent signals</span>
                    </div>
                    <div className="flex justify-between">
                      <span className="text-[#ff6d5a]">/act &lt;id&gt;</span>
                      <span className="text-gray-400 text-right">Mark alert as acted-on</span>
                    </div>
                  </div>
                </div>

                <div className="pt-4 border-t border-white/5 flex justify-end">
                  <button
                    onClick={async () => {
                      try {
                        const response = await fetch('/api/settings', {
                          method: 'POST',
                          headers: { 'Content-Type': 'application/json' },
                          body: JSON.stringify(settings)
                        });
                        if (response.ok) {
                          showToast('Settings saved successfully', 'success');
                          fetchData();
                        } else {
                          showToast('Failed to save settings', 'error');
                        }
                      } catch (e) {
                        console.error('Failed to save settings:', e);
                        showToast('Error connecting to backend', 'error');
                      }
                    }}
                    className="bg-[#ff6d5a] hover:bg-[#ff8c7a] text-black text-xs font-bold rounded-lg px-6 py-2.5 transition shadow-[0_0_15px_rgba(255,109,90,0.2)]"
                  >
                    Save Configuration
                  </button>
                </div>
              </div>
            </div>
          </div>
        )}
      </main>

      {/* Status Bar */}
      <footer className="h-7 px-4 bg-[#090b11] border-t border-white/5 flex items-center justify-between text-[10px] text-gray-500 font-mono shrink-0">
        <div>© 2026 Tachyon Ops</div>
        <div>C++ Multi-Screen Engine v3.9</div>
        <div className="flex items-center gap-3">
          <span>DB: SQLite3</span>
          <span className="text-gray-700">|</span>
          <span className="text-emerald-400/80">API Connected</span>
        </div>
      </footer>
    </div>
  </div>
  );
}

export default App;
