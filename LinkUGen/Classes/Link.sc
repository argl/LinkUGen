
LinkTempo : UGen {
	*kr { arg tempo, env;
		this.multiNew('control', tempo, env);
		^0.0;
	}
}

LinkTempoGen : UGen {
	*kr { 
		^this.multiNew('control');
	}
}

LinkEnabler : UGen {
	*kr { arg tempo = 60.0;
		this.multiNew('control', tempo);
		^0.0;
	}
}

LinkDisabler : UGen {
	*kr {
		this.multiNew('control');
		^0.0;
	}
}



Link : UGen {
	*enable { arg tempo = 60.0;
		play {
			LinkEnabler.kr(tempo);
			FreeSelf.kr(Impulse.kr(1));
		}
	}

	*disable {
		play {
			LinkDisabler.kr;
			FreeSelf.kr(Impulse.kr(1));
		}
	}

	*kr {
		arg latency = 0.0;
		^this.multiNew('control', latency);
	}

	*quantum {
		^96;
	}

	*setTempo { arg tempo, lagTime = 0.0;
		play{
			LinkTempo.kr(tempo, Line.kr(0, 1, lagTime, doneAction: 2));
		};
	}
}

LinkCount : UGen {
	*kr { arg divistion = 1, latency = 0.0;
		var count = Link.kr(latency: latency);
		var quantum = Link.quantum;
		var ratio = 1.0 / quantum;
		count = count / ratio;
		^(count / (quantum / divistion)).floor;
	}
}


LinkTrig : UGen {
	*kr { arg division = 1, latency = 0.0;
		var count = LinkCount.kr(division, latency);
		^Changed.kr(count - Latch.kr(count, 1));
	}
}


LinkLane : UGen {
	*kr { arg div = 1, max = 4, lane = [];
		^Mix(lane.collect({|item|
			var isEq = (LinkCount.kr(div) % max eq: item);
			Changed.kr(isEq) * isEq;
		}));
	}
}
