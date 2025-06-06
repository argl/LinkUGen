
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
	*kr { arg tempo = 60.0, latency=0;
		this.multiNew('control', tempo, latency);
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
	*enable { arg tempo = 60.0, latency=0;
		play {
			LinkEnabler.kr(tempo, latency);
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
		^this.multiNew('control');
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
	*kr { arg divistion = 1, offset = 0.0, latency = 0.0;
		var count = Link.kr(latency: latency) + offset;
		var quantum = Link.quantum;
		var ratio = 1.0 / quantum;
		count = count / ratio;
		^(count / (quantum / divistion)).floor;
	}
}


LinkTrig : UGen {
	*kr { arg division = 1, offset=0.0, latency = 0.0;
		var count = LinkCount.kr(division, offset, latency);
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

LinkGrid : MultiOutUGen {
	*kr { arg enabled = 0, gridSize = 4.0, beats = 16.0;
		^this.multiNew('control', enabled, gridSize, beats);
	}

	init { arg ... theInputs;
		inputs = theInputs;
		^this.initOutputs(10, 'control');
	}
}

// Convenience class for easier LinkGrid usage
LinkGridSequencer {
	*kr { arg enabled = 0, gridSize = 4, beats = 16;
		var grid = LinkGrid.kr(enabled, gridSize, beats);
		^(
			gridTrig: grid[0],
			beatTrig: grid[1],
			enabledEnv: grid[2],
			signalTrig: grid[3],
            phase: grid[4]
			signalEnv: grid[5],
			done: grid[6],
			state: grid[7],
			sigEnvLength: grid[8],
			beat: grid[9]
		);
	}
}
