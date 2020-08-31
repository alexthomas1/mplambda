#include <mpl/demo/app_options.hpp>
#include <mpl/demo/se3_rigid_body_scenario.hpp>
#include <mpl/demo/fetch_scenario.hpp>
#include <mpl/prrt.hpp>
#include <mpl/comm.hpp>
#include <mpl/pcforest.hpp>
#include <mpl/option.hpp>
#include <client/kvs_client.hpp>
#include <getopt.h>
#include <iostream>
#include <optional>

// these static variables are needed by Anna
ZmqUtil zmq_util;
ZmqUtilInterface *kZmqUtil = &zmq_util;

namespace mpl::demo {

    template <class T, class U>
    struct ConvertState : std::false_type {};

    template <class R, class S>
    struct ConvertState<
        std::tuple<Eigen::Quaternion<R>, Eigen::Matrix<R, 3, 1>>,
        std::tuple<Eigen::Quaternion<S>, Eigen::Matrix<S, 3, 1>>>
        : std::true_type {
        using Result = std::tuple<Eigen::Quaternion<R>, Eigen::Matrix<R, 3, 1>>;
        using Source = std::tuple<Eigen::Quaternion<S>, Eigen::Matrix<S, 3, 1>>;
        
        static Result apply(const Source& q) {
            return Result(
                std::get<0>(q).template cast<R>(),
                std::get<1>(q).template cast<R>());
        }
    };

    template <class R, class S, int dim>
    struct ConvertState<Eigen::Matrix<R, dim, 1>, Eigen::Matrix<S, dim, 1>>
        : std::true_type {
        using Result = Eigen::Matrix<R, dim, 1>;
        using Source = Eigen::Matrix<S, dim, 1>;

        static Result apply(const Source& q) {
            return q.template cast<R>();
        }
    };

    template <class T, class Rep, class Period>
    void sendPath(
        KvsClient* client, const std::string& solutionPathKey,
        std::chrono::duration<Rep, Period> elapsed, T& solution) {
        using State = typename T::State;
        using Distance = typename T::Distance;
        
        std::vector<State> path;
        solution.visit([&] (const State& q) { path.push_back(q); });
        std::reverse(path.begin(), path.end());
        
        Distance cost = solution.cost();
        JI_LOG(INFO) << "putting path with cost " << cost;
        // comm.sendPath(cost, elapsed, std::move(path));
        std::uint32_t elapsedMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        packet::Path<State> packet(cost, elapsedMillis, std::move(path));
        Buffer buf = packet;
        Key k = solutionPathKey;
        TopKPriorityLattice<double, string, kNumShortestPaths> top_k_priority_lattice(std::set<PriorityValuePair<double, string>>({PriorityValuePair<double, string>(cost, buf.getString())}));
        string rid = client->put_async(k, serialize(top_k_priority_lattice), LatticeType::TOPK_PRIORITY);
    }


    template <class T, class Rep, class Period>
    void sendPath(Comm& comm, std::chrono::duration<Rep, Period> elapsed, T& solution) {
        using State = typename T::State;
        using Distance = typename T::Distance;
        
        if (comm) {
            std::vector<State> path;
            solution.visit([&] (const State& q) { path.push_back(q); });
            std::reverse(path.begin(), path.end());
            Distance cost = solution.cost();
            comm.sendPath(cost, elapsed, std::move(path));
        }
    }

    template <class Scenario, class Algorithm, class ... Args>
    void runPlanner(const demo::AppOptions& options, Args&& ... args) {
        using State = typename Scenario::State;
        using Distance = typename Scenario::Distance;

        JI_LOG(INFO) << "Start function runplanner";
        State qStart = options.start<State>();


        std::vector<UserRoutingThread> threads;
        Address addr(options.anna_address_);
        for (unsigned tid = 0; tid < 4; tid ++) {
          threads.push_back(UserRoutingThread(addr, tid));
        }
        Address ip(options.local_ip_);
        int thread_id = options.thread_id_;
        std::cout << "thread id is " << thread_id << "\n";
        std::cout << "anna address is " << options.anna_address_ << "\n";
        std::cout << "local ip is " << options.local_ip_ << "\n";
        std::cout << "execution id is " << options.execution_id_ << "\n";

        JI_LOG(INFO) << "before kvsClient";
        KvsClient kvsClient(threads, ip, thread_id, 10000);
        JI_LOG(INFO) << "After kvsclient";

        if (options.coordinator(false).empty()) {
            JI_LOG(WARN) << "no coordinator set";
        }

        JI_LOG(INFO) << "setting up planner";
        Planner<Scenario, Algorithm> planner(std::forward<Args>(args)...);

        JI_LOG(INFO) << "Adding start state: " << qStart;
        planner.addStart(qStart);

        JI_LOG(INFO) << "Starting solve()";
        using Clock = std::chrono::steady_clock;
        Clock::duration maxElapsedSolveTime = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(options.timeLimit()));
        auto start = Clock::now();

        // record the initial solution (it should not be an actual
        // solution).  We use this later to perform the C-FOREST path
        // update, and to check if we should write out one last
        // solution.
        auto solution = planner.solution();
        JI_LOG(INFO) << "judge whether found a solution";
        assert(!solution);

        JI_LOG(INFO) << "found a solution";
        const std::string solutionPathKey = options.execution_id_;

        JI_LOG(INFO) << "Sync solution key" << solutionPathKey;
        kvsClient.get_async(solutionPathKey);

        if constexpr (Algorithm::asymptotically_optimal) {                
            // asymptotically-optimal planner, run for the
            // time-limit, and update the graph with best paths
            // from the network.
            planner.solve([&] {
                if (maxElapsedSolveTime.count() > 0 && Clock::now() - start > maxElapsedSolveTime)
                    return true;
                std::vector<KeyResponse> responses = kvsClient.receive_async();
                int currentTopKLength = -1;
                if (!responses.empty()) {
                    //JI_LOG(INFO) << "processing " << responses.size() << " async responses";
                    bool getResponse = false;
                    for (KeyResponse& resp : responses) {
                        if (resp.type() == RequestType::PUT) {
                            //JI_LOG(INFO) << "received PUT response for key " << resp.tuples(0).key() << " with error number " << resp.tuples(0).error();
                            continue;
                        }
                        //JI_LOG(INFO) << "received GET response for key " << resp.tuples(0).key() << " with error number " << resp.tuples(0).error();
                        getResponse = true;
                                                
                        TopKPriorityLattice<double, string, kNumShortestPaths> top_k_priority_lattice = deserialize_top_k_priority(resp.tuples(0).payload());
                        if(currentTopKLength !=  top_k_priority_lattice.reveal().size()){
                            JI_LOG(WARN) << "TopK length = " <<  top_k_priority_lattice.reveal().size();
                            currentTopKLength =  top_k_priority_lattice.reveal().size();
                        }
                        

                        for (const auto& pv : top_k_priority_lattice.reveal()) {
                          
                          Buffer buf(pv.value);
                          packet::parse(
                              buf,
                              [&] (auto&& path) {
                                  if constexpr (std::is_same_v<std::decay_t<decltype(path)>, packet::Path<State>>) {
                                    std::cerr  << path.cost() << ",";
                                      planner.addPath(path.cost(), path.path());

                                      // update our best solution if it has
                                      // the same cost as the solution we
                                      // just got from a peer.  If we have a
                                      // different solution, then we'll
                                      // update and send the solution after
                                      // the comm_.process().  This avoids
                                      // re-broadcasting the same solution.
                                      // (It is possible that incorporating
                                      // the new solution will lower the
                                      // cost)
                                      auto newSol = planner.solution();
                                      if (newSol.cost() == path.cost())
                                          solution = newSol;
                                  } else {
                                      JI_LOG(WARN) << "received invalid path type!";
                                  }
                          });
                        }
                    }
                    
                    if (getResponse) {
                        kvsClient.get_async(solutionPathKey);
                        //JI_LOG(INFO) << "kvsClient.get_async(solutionPathKey) called.";
                    }

                }
                
                auto s = planner.solution();
                //JI_LOG(INFO) << "Get solution with cost " << solution.cost();
                
                if (s < solution) {
                    sendPath(&kvsClient, solutionPathKey, Clock::now() - start, s);
                    solution = s;
                    JI_LOG(INFO) << "solution length = " << solution.cost();
                    JI_LOG(INFO) << "Get solution with cost " << solution.cost() << "," << (Clock::now() - start);
                }
                return false;
            });
        } else {
            // non-asymptotically-optimal.  Stop as soon as we
            // have a solution (either locally or from the
            // network)
            bool hasPath = false;
            planner.solve([&] {
                if (maxElapsedSolveTime.count() > 0 && Clock::now() - start > maxElapsedSolveTime)
                    return true;

                if (!hasPath) {
                    std::vector<KeyResponse> responses = kvsClient.receive_async();
                    if (!responses.empty()) {
                        // TODO: check if there's a path, if so, we're done
                        // hasPath = true;
                        kvsClient.get_async(solutionPathKey);
                    }
                }

                return hasPath || planner.isSolved();
            });
        }
            
        
        JI_LOG(INFO) << "solution " << (planner.isSolved() ? "" : "not ") << "found after " << (Clock::now() - start);
        JI_LOG(INFO) << "graph size = " << planner.size();
        JI_LOG(INFO) << "samples (goal-biased, rejected) = " << planner.samplesConsidered() << " ("
                     << planner.goalBiasedSamples() << ", "
                     << planner.rejectedSamples() << ")";
            
        if (auto finalSolution = planner.solution()) {
            if (finalSolution != solution)
                sendPath(&kvsClient, solutionPathKey, Clock::now() - start, finalSolution);
                // sendPath(comm_, Clock::now() - start, finalSolution);
            finalSolution.visit([] (const State& q) { JI_LOG(INFO) << "  " << q; });
        }
    }



    template <class Algorithm, class S>
    void runSelectScenario(const demo::AppOptions& options) {
        JI_LOG(INFO) << "running scenario: " << options.scenario();
        if (options.scenario() == "se3") {
            using Scenario = mpl::demo::SE3RigidBodyScenario<S>;
            using Bound = typename Scenario::Bound;
            using State = typename Scenario::State;
            State goal = options.goal<State>();
            Bound min = options.min<Bound>();
            Bound max = options.max<Bound>();
            runPlanner<Scenario, Algorithm>(
                options, options.env(), options.robot(), goal, min, max,
                options.checkResolution(0.1));
        } else if (options.scenario() == "fetch") {
            using Scenario = mpl::demo::FetchScenario<S>;
            using State = typename Scenario::State;
            using Frame = typename Scenario::Frame;
            using GoalRadius = Eigen::Matrix<S, 6, 1>;
            Frame envFrame = options.envFrame<Frame>();
            Frame goal = options.goal<Frame>();
            GoalRadius goalRadius = options.goalRadius<GoalRadius>();
            JI_LOG(INFO) << "Env frame: " << envFrame;
            JI_LOG(INFO) << "Goal: " << goal;
            goal = envFrame * goal;
            JI_LOG(INFO) << "Goal in robot's frame: " << goal;
            runPlanner<Scenario, Algorithm>(
                options, envFrame, options.env(), goal, goalRadius,
                options.checkResolution(0.1));
        } else {
            throw std::invalid_argument("bad scenario: " + options.scenario());
        }
    }

    template <class Algorithm>
    void runSelectPrecision(const demo::AppOptions& options) {
        // TODO: revert this.  For now keeping it out the float branch
        // should double the compilation speed.
        
        // if (options.singlePrecision()) {
        //     runSelectScenario<Algorithm, float>(options);
        // } else {
        JI_LOG(INFO) << "using precision: double";
        runSelectScenario<Algorithm, double>(options);
        // }
    }

    void runSelectPlanner(const demo::AppOptions& options) {
        JI_LOG(INFO) << "using planner: " << options.algorithm();
        if (options.algorithm() == "rrt")
            runSelectPrecision<mpl::PRRT>(options);
        else if (options.algorithm() == "cforest")
            runSelectPrecision<mpl::PCForest>(options);
        else
            throw std::invalid_argument("unknown algorithm: " + options.algorithm());
    }
}