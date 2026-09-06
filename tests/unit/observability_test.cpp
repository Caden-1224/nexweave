#include <cassert>
#include <cmath>
#include "../../core/observability/observability.hpp"
using namespace nexweave::observability;
int main(){
 RunManifest m; m.run_id="r1";m.git_commit="abc";m.compiler="g++";m.cmake="3.22";m.runtime="linux";m.driver="none";m.model="fake";m.config_hash="c";m.input_hash="i";m.device="wsl";m.profile="mock";m.command="run";m.start_time="2026-01-01T00:00:00Z";m.end_time="2026-01-01T00:00:01Z";
 auto ms=encode_manifest(m);assert(ms.ok());auto md=decode_manifest(*ms.value);assert(md.ok()&&md.value->profile=="mock"); m.model.clear();assert(!validate_manifest(m).ok());
 ObservationEvent e; e.request_id="req-1";e.session_id="sess-1";e.name="done";e.attributes["status"]="ok";auto es=encode_event(e);assert(es.ok());assert(decode_event(*es.value).ok());e.name.clear();assert(!validate_event(e).ok());
 MetricRecord q;q.request_id="req-1";q.session_id="sess-1";q.name="latency";q.unit="ms";q.value=12.5;auto qs=encode_metric(q);assert(qs.ok());assert(decode_metric(*qs.value).ok());q.value=std::nan("");assert(!validate_metric(q).ok());
 assert(!decode_event("{}").ok()); return 0; }
