#include "../interface/MultiDimFit.h"
#include <stdexcept>
#include <cmath>

#include "TMath.h"
#include "RooArgSet.h"
#include "RooArgList.h"
#include "RooRandom.h"
#include "RooAbsData.h"
#include "RooFitResult.h"
#include "../interface/RooMinimizerOpt.h"
#include <RooStats/ModelConfig.h>
#include "../interface/Combine.h"
#include "../interface/CascadeMinimizer.h"
#include "../interface/CloseCoutSentry.h"
#include "../interface/utils.h"

#include <Math/Minimizer.h>
#include <Math/MinimizerOptions.h>
#include <Math/QuantFuncMathCore.h>
#include <Math/ProbFunc.h>

using namespace RooStats;

MultiDimFit::Algo MultiDimFit::algo_ = None;
MultiDimFit::GridType MultiDimFit::gridType_ = G1x1;
std::vector<std::string>  MultiDimFit::poi_;
std::vector<RooRealVar *> MultiDimFit::poiVars_;
std::vector<float>        MultiDimFit::poiVals_;
RooArgList                MultiDimFit::poiList_;
float                     MultiDimFit::deltaNLL_ = 0;
unsigned int MultiDimFit::points_ = 50;
unsigned int MultiDimFit::firstPoint_ = 0;
unsigned int MultiDimFit::lastPoint_  = std::numeric_limits<unsigned int>::max();
bool MultiDimFit::floatOtherPOIs_ = false;
unsigned int MultiDimFit::nOtherFloatingPoi_ = 0;
bool MultiDimFit::fastScan_ = false;
bool MultiDimFit::loadedSnapshot_ = false;
bool MultiDimFit::hasMaxDeltaNLLForProf_ = false;
float MultiDimFit::maxDeltaNLLForProf_ = 200;
float MultiDimFit::plotPower_ = 0.5;
double MultiDimFit::contour = 1.15;

 std::vector<std::string>  MultiDimFit::specifiedNuis_;
 std::vector<RooRealVar *> MultiDimFit::specifiedVars_;
 std::vector<float>        MultiDimFit::specifiedVals_;
 RooArgList                MultiDimFit::specifiedList_;
 bool MultiDimFit::saveInactivePOI_= false;

MultiDimFit::MultiDimFit() :
    FitterAlgoBase("MultiDimFit specific options")
{
    options_.add_options()
        ("algo",  boost::program_options::value<std::string>()->default_value("none"), "Algorithm to compute uncertainties")
        ("poi,P",   boost::program_options::value<std::vector<std::string> >(&poi_), "Parameters of interest to fit (default = all)")
        ("floatOtherPOIs",   boost::program_options::value<bool>(&floatOtherPOIs_)->default_value(floatOtherPOIs_), "POIs other than the selected ones will be kept freely floating (1) or fixed (0, default)")
        ("points",  boost::program_options::value<unsigned int>(&points_)->default_value(points_), "Points to use for grid or contour scans")
        ("firstPoint",  boost::program_options::value<unsigned int>(&firstPoint_)->default_value(firstPoint_), "First point to use")
        ("lastPoint",  boost::program_options::value<unsigned int>(&lastPoint_)->default_value(lastPoint_), "Last point to use")
        ("fastScan", "Do a fast scan, evaluating the likelihood without profiling it.")
        ("maxDeltaNLLForProf",  boost::program_options::value<float>(&maxDeltaNLLForProf_)->default_value(maxDeltaNLLForProf_), "Last point to use")
	("saveSpecifiedNuis",   boost::program_options::value<std::vector<std::string> >(&specifiedNuis_), "Save specified parameters (default = none)")
	("saveInactivePOI",   boost::program_options::value<bool>(&saveInactivePOI_)->default_value(saveInactivePOI_), "Save inactive POIs in output (1) or not (0, default)")
        ("gridDistributionPower",  boost::program_options::value<float>(&plotPower_)->default_value(plotPower_), "Distribution of points around minimum in 1D grid scan. Default of 0.5 => points distributed ~ sqrt of distance from minimum.")
        ("contour", boost::program_options::value<double>(&contour)->default_value(contour),"Specify the likelihood value of the contour.")
	;
}

void MultiDimFit::applyOptions(const boost::program_options::variables_map &vm) 
{
    applyOptionsBase(vm);
    std::string algo = vm["algo"].as<std::string>();
    if (algo == "none") {
        algo_ = None;
    } else if (algo == "singles") {
        algo_ = Singles;
    } else if (algo == "cross") {
        algo_ = Cross;
    } else if (algo == "grid" || algo == "grid3x3" ) {
        algo_ = Grid; gridType_ = G1x1;
        if (algo == "grid3x3") gridType_ = G3x3;
    } else if (algo == "random") {
        algo_ = RandomPoints;
    } else if (algo == "contour2d") {
        algo_ = Contour2D;
    } else if (algo == "stitch2d") {
        algo_ = Stitch2D;
    } else if (algo == "smartscan") {
	algo_ = SmartScan;
    } else throw std::invalid_argument(std::string("Unknown algorithm: "+algo));
    fastScan_ = (vm.count("fastScan") > 0);
    hasMaxDeltaNLLForProf_ = !vm["maxDeltaNLLForProf"].defaulted();
    loadedSnapshot_ = !vm["snapshotName"].defaulted();
}

bool MultiDimFit::runSpecific(RooWorkspace *w, RooStats::ModelConfig *mc_s, RooStats::ModelConfig *mc_b, RooAbsData &data, double &limit, double &limitErr, const double *hint) { 
    // one-time initialization of POI variables, TTree branches, ...
    static int isInit = false;
    if (!isInit) { initOnce(w, mc_s); isInit = true; }

    // Get PDF
    RooAbsPdf &pdf = *mc_s->GetPdf();

    // Process POI not in list
    nOtherFloatingPoi_ = 0;
    RooLinkedListIter iterP = mc_s->GetParametersOfInterest()->iterator();
    for (RooAbsArg *a = (RooAbsArg*) iterP.Next(); a != 0; a = (RooAbsArg*) iterP.Next()) {
        if (poiList_.contains(*a)) continue;
        RooRealVar *rrv = dynamic_cast<RooRealVar *>(a);
        if (rrv == 0) { std::cerr << "MultiDimFit: Parameter of interest " << a->GetName() << " which is not a RooRealVar will be ignored" << std::endl; continue; }
        rrv->setConstant(!floatOtherPOIs_);
        if (floatOtherPOIs_) nOtherFloatingPoi_++;
    }
 
    // start with a best fit
    const RooCmdArg &constrainCmdArg = withSystematics  ? RooFit::Constrain(*mc_s->GetNuisanceParameters()) : RooCmdArg();
    std::auto_ptr<RooFitResult> res;
    if ( algo_ == Singles || !loadedSnapshot_ ){
    	res.reset(doFit(pdf, data, (algo_ == Singles ? poiList_ : RooArgList()), constrainCmdArg, false, 1, true, false)); 
    }
    if ( loadedSnapshot_ || res.get() || keepFailures_) {
        for (int i = 0, n = poi_.size(); i < n; ++i) {
            poiVals_[i] = poiVars_[i]->getVal();
        }
        if (algo_ != None) Combine::commitPoint(/*expected=*/false, /*quantile=*/1.); // otherwise we get it multiple times
    }
   

    std::auto_ptr<RooAbsReal> nll;
    if (algo_ != None && algo_ != Singles) {
        nll.reset(pdf.createNLL(data, constrainCmdArg, RooFit::Extended(pdf.canBeExtended())));
    } 
    
    //set snapshot for best fit
    if (!loadedSnapshot_) w->saveSnapshot("MultiDimFit",w->allVars());
    
    switch(algo_) {
        case None: 
            if (verbose > 0) {
                std::cout << "\n --- MultiDimFit ---" << std::endl;
                std::cout << "best fit parameter values: "  << std::endl;
                int len = poi_[0].length();
                for (int i = 0, n = poi_.size(); i < n; ++i) {
                    len = std::max<int>(len, poi_[i].length());
                }
                for (int i = 0, n = poi_.size(); i < n; ++i) {
                    printf("   %*s :  %+8.3f\n", len, poi_[i].c_str(), poiVals_[i]);
                }
            }
            break;
        case Singles: if (res.get()) doSingles(*res); break;
        case Cross: doBox(*nll, cl, "box", true); break;
        case Grid: doGrid(*nll); break;
        case RandomPoints: doRandomPoints(*nll); break;
        case Contour2D: doContour2D(*nll); break;
        case Stitch2D: doStitch2D(*nll); break;
	case SmartScan: doSmartScan(*nll); break;
    }
    
    return true;
}

void MultiDimFit::initOnce(RooWorkspace *w, RooStats::ModelConfig *mc_s) {
    RooArgSet mcPoi(*mc_s->GetParametersOfInterest());
    if (poi_.empty()) {
        RooLinkedListIter iterP = mc_s->GetParametersOfInterest()->iterator();
        for (RooAbsArg *a = (RooAbsArg*) iterP.Next(); a != 0; a = (RooAbsArg*) iterP.Next()) {
            poi_.push_back(a->GetName());
        }
    }
    for (std::vector<std::string>::const_iterator it = poi_.begin(), ed = poi_.end(); it != ed; ++it) {
        RooAbsArg *a = mcPoi.find(it->c_str());
        if (a == 0) throw std::invalid_argument(std::string("Parameter of interest ")+*it+" not in model.");
        RooRealVar *rrv = dynamic_cast<RooRealVar *>(a);
        if (rrv == 0) throw std::invalid_argument(std::string("Parameter of interest ")+*it+" not a RooRealVar.");
        poiVars_.push_back(rrv);
        poiVals_.push_back(rrv->getVal());
        poiList_.add(*rrv);
    }

    if(!specifiedNuis_.empty() && withSystematics){
	    RooArgSet mcNuis(*mc_s->GetNuisanceParameters());
	    if(specifiedNuis_.size()==1 && specifiedNuis_[0]=="all"){
		    specifiedNuis_.clear();
		    RooLinkedListIter iterN = mc_s->GetNuisanceParameters()->iterator();
		    for (RooAbsArg *a = (RooAbsArg*) iterN.Next(); a != 0; a = (RooAbsArg*) iterN.Next()) {
			    specifiedNuis_.push_back(a->GetName());
		    }
	    }
	    for (std::vector<std::string>::const_iterator it = specifiedNuis_.begin(), ed = specifiedNuis_.end(); it != ed; ++it) {
		    RooAbsArg *a = mcNuis.find(it->c_str());
		    if (a == 0) throw std::invalid_argument(std::string("Nuisance Parameter ")+*it+" not in model.");
		    if (poiList_.contains(*a)) continue;
		    RooRealVar *rrv = dynamic_cast<RooRealVar *>(a);
		    if (rrv == 0) throw std::invalid_argument(std::string("Nuisance Parameter ")+*it+" not a RooRealVar.");
		    specifiedVars_.push_back(rrv);
		    specifiedVals_.push_back(rrv->getVal());
		    specifiedList_.add(*rrv);
	    }
    }
    if(saveInactivePOI_){
	    RooLinkedListIter iterP = mc_s->GetParametersOfInterest()->iterator();
	    for (RooAbsArg *a = (RooAbsArg*) iterP.Next(); a != 0; a = (RooAbsArg*) iterP.Next()) {
		    if (poiList_.contains(*a)) continue;
		    if (specifiedList_.contains(*a)) continue;
		    RooRealVar *rrv = dynamic_cast<RooRealVar *>(a);
		    specifiedNuis_.push_back(a->GetName());
		    specifiedVars_.push_back(rrv);
		    specifiedVals_.push_back(rrv->getVal());
		    specifiedList_.add(*rrv);
	    }
    }

    // then add the branches to the tree (at the end, so there are no resizes)
    for (int i = 0, n = poi_.size(); i < n; ++i) {
        Combine::addBranch(poi_[i].c_str(), &poiVals_[i], (poi_[i]+"/F").c_str()); 
    }
    for (int i = 0, n = specifiedNuis_.size(); i < n; ++i) {
	Combine::addBranch(specifiedNuis_[i].c_str(), &specifiedVals_[i], (specifiedNuis_[i]+"/F").c_str()); 
    }
    Combine::addBranch("deltaNLL", &deltaNLL_, "deltaNLL/F");
}

void MultiDimFit::doSingles(RooFitResult &res)
{
    std::cout << "\n --- MultiDimFit ---" << std::endl;
    std::cout << "best fit parameter values and profile-likelihood uncertainties: "  << std::endl;
    int len = poi_[0].length();
    for (int i = 0, n = poi_.size(); i < n; ++i) {
        len = std::max<int>(len, poi_[i].length());
    }
    for (int i = 0, n = poi_.size(); i < n; ++i) {
	RooAbsArg *rfloat = res.floatParsFinal().find(poi_[i].c_str());
	if (!rfloat) {
		rfloat = res.constPars().find(poi_[i].c_str());
	}
        RooRealVar *rf = dynamic_cast<RooRealVar*>(rfloat);
        double bestFitVal = rf->getVal();

        double hiErr = +(rf->hasRange("err68") ? rf->getMax("err68") - bestFitVal : rf->getAsymErrorHi());
        double loErr = -(rf->hasRange("err68") ? rf->getMin("err68") - bestFitVal : rf->getAsymErrorLo());
        double maxError = std::max<double>(std::max<double>(hiErr, loErr), rf->getError());

        if (fabs(hiErr) < 0.001*maxError) hiErr = -bestFitVal + rf->getMax();
        if (fabs(loErr) < 0.001*maxError) loErr = +bestFitVal - rf->getMin();

        double hiErr95 = +(do95_ && rf->hasRange("err95") ? rf->getMax("err95") - bestFitVal : 0);
        double loErr95 = -(do95_ && rf->hasRange("err95") ? rf->getMin("err95") - bestFitVal : 0);

        poiVals_[i] = bestFitVal - loErr; Combine::commitPoint(true, /*quantile=*/0.32);
        poiVals_[i] = bestFitVal + hiErr; Combine::commitPoint(true, /*quantile=*/0.32);
        if (do95_ && rf->hasRange("err95")) {
            poiVals_[i] = rf->getMax("err95"); Combine::commitPoint(true, /*quantile=*/0.05);
            poiVals_[i] = rf->getMin("err95"); Combine::commitPoint(true, /*quantile=*/0.05);
            poiVals_[i] = bestFitVal;
            printf("   %*s :  %+8.3f   %+6.3f/%+6.3f (68%%)    %+6.3f/%+6.3f (95%%) \n", len, poi_[i].c_str(), 
                    poiVals_[i], -loErr, hiErr, loErr95, -hiErr95);
        } else {
            poiVals_[i] = bestFitVal;
            printf("   %*s :  %+8.3f   %+6.3f/%+6.3f (68%%)\n", len, poi_[i].c_str(), 
                    poiVals_[i], -loErr, hiErr);
        }
    }
}

void MultiDimFit::doGrid(RooAbsReal &nll) 
{
    unsigned int n = poi_.size();
    //if (poi_.size() > 2) throw std::logic_error("Don't know how to do a grid with more than 2 POIs.");
    double nll0 = nll.getVal();

    std::vector<double> p0(n), pmin(n), pmax(n);
    for (unsigned int i = 0; i < n; ++i) {
        p0[i] = poiVars_[i]->getVal();
        pmin[i] = poiVars_[i]->getMin();
        pmax[i] = poiVars_[i]->getMax();
        poiVars_[i]->setConstant(true);
    }

    CascadeMinimizer minim(nll, CascadeMinimizer::Constrained);
    minim.setStrategy(minimizerStrategy_);
    std::auto_ptr<RooArgSet> params(nll.getParameters((const RooArgSet *)0));
    RooArgSet snap; params->snapshot(snap);
    //snap.Print("V");
    if (n == 1) {
      	if (plotPower_>1){
	double a = pmin[0];
	double b = pmax[0];
	double x1 = 0, x2 = 0, y1 = 0, y2 = 0, d;
	unsigned int count = 1;
	bool ok;
	double precision = (pmax[0]-pmin[0])/double(points_);
	std::cout<<"Estimating minima. \n";
	// Golden section search for the minima
        for (unsigned int i = 0; i < points_/2; ++i) {
	  if ((b-a)<precision) break;            
	  if (i < firstPoint_) continue;
          if (i > lastPoint_)  break;
	  d = (b-a)/double(3);		
	  x1 = a + d;
	  x2 = b - d;
			
	  *params = snap;
	  poiVals_[0] = x1;
	  poiVars_[0]->setVal(x1);
	  minim.minimize(verbose-1);
	  y1 = nll.getVal();
			
	  if (verbose > 1) std::cout << "Point " << count << "/" << points_ << " " << poiVars_[0]->GetName() << " = " << x1 << std::endl;			

	  ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (y1 - nll0) > maxDeltaNLLForProf_) ? 
          true : 
          minim.minimize(verbose-1);
	  if (ok) {
                deltaNLL_ = nll.getVal() - nll0;
                double qN = 2*(deltaNLL_);
                double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	        for(unsigned int j=0; j<specifiedNuis_.size(); j++){
		   specifiedVals_[j]=specifiedVars_[j]->getVal();
	        }
                Combine::commitPoint(true, /*quantile=*/prob);
          }			
			
	  *params = snap;			
  	  poiVals_[0] = x2;
          poiVars_[0]->setVal(x2);
	  minim.minimize(verbose-1);			
	  y2 = nll.getVal();
	  if (verbose > 1) std::cout << "Point " << count + 1<< "/" << points_ << " " << poiVars_[0]->GetName() << " = " << x2 << std::endl;	
			
	  ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (y2 - nll0) > maxDeltaNLLForProf_) ? 
          true : 
          minim.minimize(verbose-1);
	  if (ok) {
                deltaNLL_ = nll.getVal() - nll0;
                double qN = 2*(deltaNLL_);
                double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	        for(unsigned int j=0; j<specifiedNuis_.size(); j++){
		   specifiedVals_[j]=specifiedVars_[j]->getVal();
	        }
                Combine::commitPoint(true, /*quantile=*/prob);
          }			
	  count += 2;				
	  if(y1<y2){ b = x2; }
	  else{ a = x1;}
        }
        
	if ((x2-x1) > precision ) std::cout<<"You may want to increase the number of points or decrease the range in another run to improve precision.\n";
	if ((x2 - pmin[0]) < precision || (pmax[0] - x1) < precision) std::cout<<"The minima appears to lie beyond the given range.\n";
	count -= 2;
	std::cout<<"Evaluating neighbourhood.\n";


	//now doing quadratic distribution of points around the minima
	double x;
	double xmin = (x1+x2)/2;
	unsigned int points_left = (unsigned int)((points_-count)*xmin/(pmax[0]-pmin[0]));
	unsigned int points_right = points_-count - points_left; 
		
	for (unsigned int i = 1; i < (points_right+1); ++i) {//plotting points on the right of the minima
          if (i < firstPoint_) continue;
          if (i > lastPoint_) break;

	  x = xmin+(pmax[0]-xmin)*pow(i/double(points_right),plotPower_); 

          if (verbose > 1) std::cout << "Point " << i << "/" << points_ << " " << poiVars_[0]->GetName() << " = " << x << std::endl;
          *params = snap;
          poiVals_[0] = x;
          poiVars_[0]->setVal(x);
          // now we minimize
          bool ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ?
                      true :
                      minim.minimize(verbose-1);
          if (ok) {
              deltaNLL_ = nll.getVal() - nll0;
              double qN = 2*(deltaNLL_);
              double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	      for(unsigned int j=0; j<specifiedNuis_.size(); j++){
		   specifiedVals_[j]=specifiedVars_[j]->getVal();
	      }
              Combine::commitPoint(true, /*quantile=*/prob);
          }
        }
		
        for (unsigned int i = 1; i < (points_left-1); ++i) {//plotting points on the left of the minima
          if (i < firstPoint_) continue;
          if (i > lastPoint_) break;

	  x = xmin+(pmin[0]-xmin)*pow(i/double(points_left),plotPower_); 

          if (verbose > 1) std::cout << "Point " << i << "/" << points_ << " " << poiVars_[0]->GetName() << " = " << x << std::endl;
          *params = snap;
          poiVals_[0] = x;
          poiVars_[0]->setVal(x);
          // now we minimize
          bool ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ?
                      true :
                      minim.minimize(verbose-1);
          if (ok) {
              deltaNLL_ = nll.getVal() - nll0;
              double qN = 2*(deltaNLL_);
              double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	      for(unsigned int j=0; j<specifiedNuis_.size(); j++){
		   specifiedVals_[j]=specifiedVars_[j]->getVal();
	      }
              Combine::commitPoint(true, /*quantile=*/prob);
          }
	 }
	}
	
	else if(plotPower_<1){
	 double x;
	 double xmin_default = poiVars_[0]->getVal();
	 unsigned int points_left = (unsigned int)((points_)*xmin_default/(pmax[0]-pmin[0]));
  	 unsigned int points_right = points_ - points_left;
	 //plotting points on the right
 	 for (unsigned int i = 1; i < (points_right+1); ++i) {//plotting points on the right of the minima
          if (i < firstPoint_) continue;
          if (i > lastPoint_) break;

	  	  x = pmax[0]+(xmin_default-pmax[0])*pow(i/double(points_right),plotPower_); 
		  if (x<0) std::cout<<"Problem with right.\n";
          if (verbose > 1) std::cout << "Point " << i << "/" << points_ << " " << poiVars_[0]->GetName() << " = " << x << std::endl;
          *params = snap;
          poiVals_[0] = x;
          poiVars_[0]->setVal(x);
          // now we minimize
          bool ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ?
                      true :
                      minim.minimize(verbose-1);
          if (ok) {
              deltaNLL_ = nll.getVal() - nll0;
              double qN = 2*(deltaNLL_);
              double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	      for(unsigned int j=0; j<specifiedNuis_.size(); j++){
		   specifiedVals_[j]=specifiedVars_[j]->getVal();
	      }
              Combine::commitPoint(true, /*quantile=*/prob);
          }
        }
		
        for (unsigned int i = 1; i < (points_left+1); ++i) {//plotting points on the left of the minima
          if (i < firstPoint_) continue;
          if (i > lastPoint_) break;

	  	  x = pmin[0]+(xmin_default-pmin[0])*pow(i/double(points_left),plotPower_); 
		  if (x<0) std::cout<<"Problem with left.\n";
          if (verbose > 1) std::cout << "Point " << i << "/" << points_ << " " << poiVars_[0]->GetName() << " = " << x << std::endl;
          *params = snap;
          poiVals_[0] = x;
          poiVars_[0]->setVal(x);
          // now we minimize
          bool ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ?
                      true :
                      minim.minimize(verbose-1);
          if (ok) {
              deltaNLL_ = nll.getVal() - nll0;
              double qN = 2*(deltaNLL_);
              double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	      for(unsigned int j=0; j<specifiedNuis_.size(); j++){
		   specifiedVals_[j]=specifiedVars_[j]->getVal();
	      }
              Combine::commitPoint(true, /*quantile=*/prob);
          }
	 }
	
	 

	}
	else{
	 // linear distribution
         for (unsigned int i = 0; i < points_; ++i) {
            if (i < firstPoint_) continue;
            if (i > lastPoint_)  break;
            double x =  pmin[0] + (i+0.5)*(pmax[0]-pmin[0])/points_;
            if (verbose > 1) std::cout << "Point " << i << "/" << points_ << " " << poiVars_[0]->GetName() << " = " << x << std::endl;
            *params = snap; 
            poiVals_[0] = x;
            poiVars_[0]->setVal(x);
            // now we minimize
            bool ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ? 
                        true : 
                        minim.minimize(verbose-1);
            if (ok) {
                deltaNLL_ = nll.getVal() - nll0;
                double qN = 2*(deltaNLL_);
                double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
		 for(unsigned int j=0; j<specifiedNuis_.size(); j++){
			specifiedVals_[j]=specifiedVars_[j]->getVal();
		}
                Combine::commitPoint(true, /*quantile=*/prob);
            }
        }
       }
    } else if (n == 2) {
        unsigned int sqrn = ceil(sqrt(double(points_)));
        unsigned int ipoint = 0, nprint = ceil(0.005*sqrn*sqrn);
        RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::CountErrors);
        CloseCoutSentry sentry(verbose < 2);
        double deltaX =  (pmax[0]-pmin[0])/sqrn, deltaY = (pmax[1]-pmin[1])/sqrn;
        for (unsigned int i = 0; i < sqrn; ++i) {
            for (unsigned int j = 0; j < sqrn; ++j, ++ipoint) {
                if (ipoint < firstPoint_) continue;
                if (ipoint > lastPoint_)  break;
                *params = snap; 
                double x =  pmin[0] + (i+0.5)*deltaX; 
                double y =  pmin[1] + (j+0.5)*deltaY; 
                if (verbose && (ipoint % nprint == 0)) {
                         fprintf(sentry.trueStdOut(), "Point %d/%d, (i,j) = (%d,%d), %s = %f, %s = %f\n",
                                        ipoint,sqrn*sqrn, i,j, poiVars_[0]->GetName(), x, poiVars_[1]->GetName(), y);
                }
                poiVals_[0] = x;
                poiVals_[1] = y;
                poiVars_[0]->setVal(x);
                poiVars_[1]->setVal(y);
                nll.clearEvalErrorLog(); nll.getVal();
                if (nll.numEvalErrors() > 0) { 
			for(unsigned int j=0; j<specifiedNuis_.size(); j++){
				specifiedVals_[j]=specifiedVars_[j]->getVal();
			}
                    deltaNLL_ = 9999; Combine::commitPoint(true, /*quantile=*/0); 
                    if (gridType_ == G3x3) {
                        for (int i2 = -1; i2 <= +1; ++i2) {
                            for (int j2 = -1; j2 <= +1; ++j2) {
                                if (i2 == 0 && j2 == 0) continue;
                                poiVals_[0] = x + 0.33333333*i2*deltaX;
                                poiVals_[1] = y + 0.33333333*j2*deltaY;
				for(unsigned int j=0; j<specifiedNuis_.size(); j++){
					specifiedVals_[j]=specifiedVars_[j]->getVal();
				}
                                deltaNLL_ = 9999; Combine::commitPoint(true, /*quantile=*/0); 
                            }
                        }
                    }
                    continue;
                }
                // now we minimize
                bool skipme = hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_;
                bool ok = fastScan_ || skipme ? true :  minim.minimize(verbose-1);
                if (ok) {
                    deltaNLL_ = nll.getVal() - nll0;
                    double qN = 2*(deltaNLL_);
                    double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
		    for(unsigned int j=0; j<specifiedNuis_.size(); j++){
			    specifiedVals_[j]=specifiedVars_[j]->getVal();
		    }
                    Combine::commitPoint(true, /*quantile=*/prob);
                }
                if (gridType_ == G3x3) {
                    bool forceProfile = !fastScan_ && std::min(fabs(deltaNLL_ - 1.15), fabs(deltaNLL_ - 2.995)) < 0.5;
                    utils::CheapValueSnapshot center(*params);
                    double x0 = x, y0 = y;
                    for (int i2 = -1; i2 <= +1; ++i2) {
                        for (int j2 = -1; j2 <= +1; ++j2) {
                            if (i2 == 0 && j2 == 0) continue;
                            center.writeTo(*params);
                            x = x0 + 0.33333333*i2*deltaX;
                            y = y0 + 0.33333333*j2*deltaY;
                            poiVals_[0] = x; poiVars_[0]->setVal(x);
                            poiVals_[1] = y; poiVars_[1]->setVal(y);
                            nll.clearEvalErrorLog(); nll.getVal();
                            if (nll.numEvalErrors() > 0) { 
				    for(unsigned int j=0; j<specifiedNuis_.size(); j++){
					    specifiedVals_[j]=specifiedVars_[j]->getVal();
				    }
                                deltaNLL_ = 9999; Combine::commitPoint(true, /*quantile=*/0); 
                                continue;
                            }
                            deltaNLL_ = nll.getVal() - nll0;
                            if (forceProfile || (!fastScan_ && std::min(fabs(deltaNLL_ - 1.15), fabs(deltaNLL_ - 2.995)) < 0.5)) {
                                minim.minimize(verbose-1);
                                deltaNLL_ = nll.getVal() - nll0;
                            }
                            double qN = 2*(deltaNLL_);
                            double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
			    for(unsigned int j=0; j<specifiedNuis_.size(); j++){
				    specifiedVals_[j]=specifiedVars_[j]->getVal();
			    }
                            Combine::commitPoint(true, /*quantile=*/prob);
                        }
                    }
                }
            }
        }

    } else { // Use utils routine if n > 2 

        unsigned int rootn = ceil(TMath::Power(double(points_),double(1./n)));
        unsigned int ipoint = 0, nprint = ceil(0.005*TMath::Power((double)rootn,(double)n));
	
        RooAbsReal::setEvalErrorLoggingMode(RooAbsReal::CountErrors);
        CloseCoutSentry sentry(verbose < 2);
	
	// Create permutations 
        std::vector<int> axis_points;
	
        for (unsigned int poi_i=0;poi_i<n;poi_i++){
	  axis_points.push_back((int)rootn);
    	}

        std::vector<std::vector<int> > permutations = utils::generateCombinations(axis_points);
	// Step through points
        std::vector<std::vector<int> >::iterator perm_it = permutations.begin();
	int npermutations = permutations.size();
    	for (;perm_it!=permutations.end(); perm_it++){

          if (ipoint < firstPoint_) continue;
          if (ipoint > lastPoint_)  break;
          *params = snap; 

          if (verbose && (ipoint % nprint == 0)) {
             fprintf(sentry.trueStdOut(), "Point %d/%d, ",
                          ipoint,npermutations);
          }	  
          for (unsigned int poi_i=0;poi_i<n;poi_i++){
	    int ip = (*perm_it)[poi_i];
            double deltaXi = (pmax[poi_i]-pmin[poi_i])/rootn;
	    double xi = pmin[poi_i]+deltaXi*(ip+0.5);
            poiVals_[poi_i] = xi; poiVars_[poi_i]->setVal(xi);
	    if (verbose && (ipoint % nprint == 0)){
             fprintf(sentry.trueStdOut(), " %s = %f ",
                          poiVars_[poi_i]->GetName(), xi);
	    }
	  }
	  if (verbose && (ipoint % nprint == 0)) fprintf(sentry.trueStdOut(), "\n");

          nll.clearEvalErrorLog(); nll.getVal();
          if (nll.numEvalErrors() > 0) { 
		for(unsigned int j=0; j<specifiedNuis_.size(); j++){
			specifiedVals_[j]=specifiedVars_[j]->getVal();
		}
               deltaNLL_ = 9999; Combine::commitPoint(true, /*quantile=*/0);
	       continue;
	  }
          // now we minimize
          bool skipme = hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_;
          bool ok = fastScan_ || skipme ? true :  minim.minimize(verbose-1);
          if (ok) {
               deltaNLL_ = nll.getVal() - nll0;
               double qN = 2*(deltaNLL_);
               double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
		for(unsigned int j=0; j<specifiedNuis_.size(); j++){
			specifiedVals_[j]=specifiedVars_[j]->getVal();
		}
               Combine::commitPoint(true, /*quantile=*/prob);
          }
	  ipoint++;	
	} 
    }
}

void MultiDimFit::doRandomPoints(RooAbsReal &nll) 
{






}

void MultiDimFit::doContour2D(RooAbsReal &nll) 
{
    if (poi_.size() != 2) throw std::logic_error("Contour2D works only in 2 dimensions");
    RooRealVar *xv = poiVars_[0]; double x0 = poiVals_[0]; float &x = poiVals_[0];
    RooRealVar *yv = poiVars_[1]; double y0 = poiVals_[1]; float &y = poiVals_[1];

    double threshold = nll.getVal() + 0.5*ROOT::Math::chisquared_quantile_c(1-cl,2+nOtherFloatingPoi_);
    if (verbose>0) std::cout << "Best fit point is for " << xv->GetName() << ", "  << yv->GetName() << " =  " << x0 << ", " << y0 << std::endl;

    // make a box
    doBox(nll, cl, "box");
    double xMin = xv->getMin("box"), xMax = xv->getMax("box");
    double yMin = yv->getMin("box"), yMax = yv->getMax("box");

    verbose--; // reduce verbosity to avoid messages from findCrossing
    // ===== Get relative min/max of x for several fixed y values =====
    yv->setConstant(true);
    for (unsigned int j = 0; j <= points_; ++j) {
        if (j < firstPoint_) continue;
        if (j > lastPoint_)  break;
        // take points uniformly spaced in polar angle in the case of a perfect circle
        double yc = 0.5*(yMax + yMin), yr = 0.5*(yMax - yMin);
        yv->setVal( yc + yr * std::cos(j*M_PI/double(points_)) );
        // ===== Get the best fit x (could also do without profiling??) =====
        xv->setConstant(false);  xv->setVal(x0);
        CascadeMinimizer minimXI(nll, CascadeMinimizer::Unconstrained, xv);
        minimXI.setStrategy(minimizerStrategy_);
        {
            CloseCoutSentry sentry(verbose < 3);    
            minimXI.minimize(verbose-1);
        }
        double xc = xv->getVal(); xv->setConstant(true);
        if (verbose>-1) std::cout << "Best fit " << xv->GetName() << " for  " << yv->GetName() << " = " << yv->getVal() << " is at " << xc << std::endl;
        // ===== Then get the range =====
        CascadeMinimizer minim(nll, CascadeMinimizer::Constrained);
        double xup = findCrossing(minim, nll, *xv, threshold, xc, xMax);
        if (!std::isnan(xup)) { 
            x = xup; y = yv->getVal(); Combine::commitPoint(true, /*quantile=*/1-cl);
            if (verbose>-1) std::cout << "Minimum of " << xv->GetName() << " at " << cl << " CL for " << yv->GetName() << " = " << y << " is " << x << std::endl;
        }
        
        double xdn = findCrossing(minim, nll, *xv, threshold, xc, xMin);
        if (!std::isnan(xdn)) { 
            x = xdn; y = yv->getVal(); Combine::commitPoint(true, /*quantile=*/1-cl);
            if (verbose>-1) std::cout << "Maximum of " << xv->GetName() << " at " << cl << " CL for " << yv->GetName() << " = " << y << " is " << x << std::endl;
        }
    }

    verbose++; // restore verbosity
}




void MultiDimFit::doStitch2D(RooAbsReal &nll)
{
    unsigned int n = poi_.size();
    unsigned int sectors = 4;
    if (n==2){
	bool ok;
    	double nll0 = nll.getVal();

    	std::vector<double> p0(n), pmin(n), pmax(n);
    	for (unsigned int i = 0; i < n; ++i) {
            p0[i] = poiVars_[i]->getVal();
            pmin[i] = poiVars_[i]->getMin();
            pmax[i] = poiVars_[i]->getMax();
            poiVars_[i]->setConstant(true);
    	}

    	CascadeMinimizer minim(nll, CascadeMinimizer::Constrained);
    	minim.setStrategy(minimizerStrategy_);
    	std::auto_ptr<RooArgSet> params(nll.getParameters((const RooArgSet *)0));
	
	
	const double pi = 3.14159;

	double set_level = contour; 
	double step= pow((pmax[0]-pmin[0])*(pmax[1]-pmin[1])/double(points_),0.5);
	const double x0 = poiVars_[0]->getVal(), y0 = poiVars_[1]->getVal(); 
	for(unsigned int u=0; u<sectors; u++){
 	    double x = x0, y = y0;
 	    std::cout<<"Job#"<<u+1<<"\n"<<"Starting from ("<<x0<<","<<y0<<"), moving outwards to touch contour."<<std::endl;	
 	    	
 	    const double theta_min = u*2*pi/sectors, theta_max = (u+1)*2*pi/sectors;
 	    
 	    /*Finding the edge on the line where the line intersects the bundary*/
 	    double r=step, rmax, rmin=r, l1=-set_level, l2;
 	    while(r*cos(theta_min)>pmin[0] && r*sin(theta_min)>pmin[1] && r*cos(theta_min)<pmax[0] && r*sin(theta_min)<pmax[1]) r+= step;
 	    rmax=r;
 	    	
 	    x = x0 + rmax*cos(theta_min);
 	    y = y0 + rmax*sin(theta_min);
 	        
 	    poiVals_[0] = x; poiVals_[1] = y;
 	    poiVars_[0]->setVal(x); poiVars_[1]->setVal(y);
 	    ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ? true :minim.minimize(verbose-1);
  	    if (ok) {
 	        deltaNLL_ = nll.getVal() - nll0;
 	        double qN = 2*(deltaNLL_);
    	        double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
    	        for(unsigned int j=0; j<specifiedNuis_.size(); j++){
 	            specifiedVals_[j]=specifiedVars_[j]->getVal();
    	        }
    	        Combine::commitPoint(true,prob);
 	    }	
 	    l2 = deltaNLL_-set_level;
 	    if(l2<0) {
 	        std::cout<<"Please change the range so that the contour is enclosed completely by it.\n";
 	    }
 	    if(l1>0) {
 	       std::cout<<"Only positive values for contours accepted.\n";
 	    } 
 	    /*Starting bisection method to find the point on the contour*/
	    unsigned int enough = 0;	
 	    while((rmax-rmin)>step){
 	        x = x0 + (rmax+rmin)*cos(theta_min)/2;
 	        y = y0 + (rmax+rmin)*sin(theta_min)/2;
 	        
 	        poiVals_[0] = x; poiVals_[1] = y;
 	        poiVars_[0]->setVal(x); poiVars_[1]->setVal(y);
 	        ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ? true :minim.minimize(verbose-1);
  	        if (ok) {
 	    	    deltaNLL_ = nll.getVal() - nll0;
 	    	    double qN = 2*(deltaNLL_);
    	    	    double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
    	    	    for(unsigned int j=0; j<specifiedNuis_.size(); j++){
 	    		specifiedVals_[j]=specifiedVars_[j]->getVal();
    	    	    }
    	    	    Combine::commitPoint(true,prob);
 	        }
 	        
		if((deltaNLL_-set_level) < 0){
 	    	    rmin = (rmax+rmin)/2;
 	    	    l1 = deltaNLL_ - set_level;
 	        }
 	        else{
 	    	    rmax = (rmax+rmin)/2;
 	    	    l2 = deltaNLL_ - set_level;
 	        }
 	       

		enough++;
 	        if(enough>points_/sectors) {
 	    	    std::cout<<"Bisection method to reach the contour starting from the interior point did not converge.\n";
 	    	    break;
 	        }
	    }
 	 		
 	 


	
	const double x_start = x0 + (rmax+rmin)*cos(theta_min)/2, y_start = y0 + (rmax+rmin)*sin(theta_min)/2;

	int cost1=0;
	double theta=-99999, theta_old = theta;
	double l=2.828427*pi*(rmax+rmin)/(points_);
	
	double x1, y1, z1, x2, y2, z2, X, Y;
	double alpha=pi/4;		
	
	std::cout<<"Touched countour at ("<<x_start<<","<<y_start<<")"<<std::endl;
	std::cout<<"Probe length being used: "<<l<<". Decrease granularity to decrease probe length if this is too small."<<std::endl;

/****************************************Starting stitching*********************************************************/
	x = x_start;
	y = y_start;
	while(theta<theta_max){
		theta = atan2(y-y0,x-x0);
		if(theta<0) theta += 2*pi;
		//std::cout<<theta<<std::endl;
		x1 = x - l*cos(theta- alpha);
		y1 = y - l*sin(theta-alpha);
		poiVals_[0] = x1; poiVals_[1] = y1;
		poiVars_[0]->setVal(x1); poiVars_[1]->setVal(y1);
		
		ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ? true :minim.minimize(verbose-1);
		if (ok) {
		   cost1++;
		   deltaNLL_ = nll.getVal() - nll0;
           	   double qN = 2*(deltaNLL_);
           	   double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	   	   for(unsigned int j=0; j<specifiedNuis_.size(); j++){
			specifiedVals_[j]=specifiedVars_[j]->getVal();
	   	   }
           	   Combine::commitPoint(true,prob);
        	}
		
		z1 = deltaNLL_-set_level;


		x2 = x + l*cos(theta+alpha);
		y2 = y + l*sin(theta+ alpha);
		poiVals_[0] = x2; poiVals_[1] = y2;
		poiVars_[0]->setVal(x2); poiVars_[1]->setVal(y2);
		
		ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ? true :minim.minimize(verbose-1);
		if (ok) {
		   cost1++;
		   deltaNLL_ = nll.getVal() - nll0;
        	   double qN = 2*(deltaNLL_);
           	   double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	   	   for(unsigned int j=0; j<specifiedNuis_.size(); j++){
			specifiedVals_[j]=specifiedVars_[j]->getVal();
	   	   }
           	   Combine::commitPoint(true,prob);
        	}
	
		z2 = deltaNLL_- set_level;

		X = x1+(x2-x1)*z1/(z1-z2);
		Y = y1 +(y2-y1)*z1/(z1-z2);

		if(theta>theta_old){
		   x = X;
		   y = Y;
	  	   poiVals_[0] = X; poiVals_[1] = Y;
		   poiVars_[0]->setVal(X); poiVars_[1]->setVal(Y);
			
		   deltaNLL_=set_level;
		   double qN = 2*(deltaNLL_);
           	   double prob = ROOT::Math::chisquared_cdf_c(qN, n+nOtherFloatingPoi_);
	   	   for(unsigned int j=0; j<specifiedNuis_.size(); j++){
				specifiedVals_[j]=specifiedVars_[j]->getVal();
	   	    }
           	   Combine::commitPoint(true,prob);
		}

		
		else break;
		
		theta_old=theta;
	}
	std::cout<<"Points traced:"<<cost1<<std::endl;

      }
		
	
    }
    else std::cout<<"Stitch2D works only in 2 dimensions.\n";


}




void MultiDimFit::doBox(RooAbsReal &nll, double cl, const char *name, bool commitPoints)  {
    unsigned int n = poi_.size();
    double nll0 = nll.getVal(), threshold = nll0 + 0.5*ROOT::Math::chisquared_quantile_c(1-cl,n+nOtherFloatingPoi_);

    std::vector<double> p0(n);
    for (unsigned int i = 0; i < n; ++i) {
        p0[i] = poiVars_[i]->getVal();
        poiVars_[i]->setConstant(false);
    }

    verbose--; // reduce verbosity due to findCrossing
    for (unsigned int i = 0; i < n; ++i) {
        RooRealVar *xv = poiVars_[i];
        xv->setConstant(true);
        CascadeMinimizer minimX(nll, CascadeMinimizer::Constrained);
        minimX.setStrategy(minimizerStrategy_);

        for (unsigned int j = 0; j < n; ++j) poiVars_[j]->setVal(p0[j]);
        double xMin = findCrossing(minimX, nll, *xv, threshold, p0[i], xv->getMin()); 
        if (!std::isnan(xMin)) { 
            if (verbose > -1) std::cout << "Minimum of " << xv->GetName() << " at " << cl << " CL for all others floating is " << xMin << std::endl;
            for (unsigned int j = 0; j < n; ++j) poiVals_[j] = poiVars_[j]->getVal();
            if (commitPoints) Combine::commitPoint(true, /*quantile=*/1-cl);
        } else {
            xMin = xv->getMin();
            for (unsigned int j = 0; j < n; ++j) poiVals_[j] = poiVars_[j]->getVal();
            double prob = ROOT::Math::chisquared_cdf_c(2*(nll.getVal() - nll0), n+nOtherFloatingPoi_);
            if (commitPoints) Combine::commitPoint(true, /*quantile=*/prob);
            if (verbose > -1) std::cout << "Minimum of " << xv->GetName() << " at " << cl << " CL for all others floating is " << xMin << " (on the boundary, p-val " << prob << ")" << std::endl;
        }
        
        for (unsigned int j = 0; j < n; ++j) poiVars_[j]->setVal(p0[j]);
        double xMax = findCrossing(minimX, nll, *xv, threshold, p0[i], xv->getMax()); 
        if (!std::isnan(xMax)) { 
            if (verbose > -1) std::cout << "Maximum of " << xv->GetName() << " at " << cl << " CL for all others floating is " << xMax << std::endl;
            for (unsigned int j = 0; j < n; ++j) poiVals_[j] = poiVars_[j]->getVal();
            if (commitPoints) Combine::commitPoint(true, /*quantile=*/1-cl);
        } else {
            xMax = xv->getMax();
            double prob = ROOT::Math::chisquared_cdf_c(2*(nll.getVal() - nll0), n+nOtherFloatingPoi_);
            for (unsigned int j = 0; j < n; ++j) poiVals_[j] = poiVars_[j]->getVal();
            if (commitPoints) Combine::commitPoint(true, /*quantile=*/prob);
            if (verbose > -1) std::cout << "Maximum of " << xv->GetName() << " at " << cl << " CL for all others floating is " << xMax << " (on the boundary, p-val " << prob << ")" << std::endl;
        }

        xv->setRange(name, xMin, xMax);
        xv->setConstant(false);
    }
    verbose++; // restore verbosity 
}




void MultiDimFit::doSmartScan(RooAbsReal &nll){

int D = poi_.size();
double nll0 = nll.getVal();

/************remove this*****************/

// double plotPower_ = 0.5;
// int points_ = 1000;

/****************************************/

bool ok;
std::vector<double> p0(D), pmin(D), pmax(D);
for (int i = 0; i < D; ++i) 
{
	p0[i] = poiVars_[i]->getVal();
	pmin[i] = poiVars_[i]->getMin();
	pmax[i] = poiVars_[i]->getMax();
	poiVars_[i]->setConstant(true);
}
CascadeMinimizer minim(nll, CascadeMinimizer::Constrained);
minim.setStrategy(minimizerStrategy_);
CloseCoutSentry sentry(verbose < 3);
std::auto_ptr<RooArgSet> params(nll.getParameters((const RooArgSet *)0));
std::vector<double> origin(D);
std::cout<<"The grid will be focused around the minima at: (";
for(int q=0; q<D; q++) 
{
	origin[q]=poiVars_[q]->getVal();
	std::cout<<origin[q];
	if(q!=(D-1))std::cout<<",";
}
std::cout<<")\n";

int points = int(pow(points_,1/double(D)));
std::cout<<points <<" points in each dimension.\n"; 

/*calcluate points on each side*/
std::vector<int> points_left(D), points_right(D);
for(int q=0; q<D; q++)
{
	points_left[q]= (int)((points)*(origin[q]-pmin[q])/(pmax[q]-pmin[q]));
	points_right[q] = points- points_left[q];
}


/*Begin plotting*/
int index;
std::vector<double> x(D);
// std::cout<<"(";
for(int i=0; i<pow(points,D); i++)
{
	for(int j=1; j<=D; j++)
	{
		index = (i/int(pow(points,j-1))%points)-points_left[j-1];
		
		/*plotting points on the right of the minimum*/
		if(index>0)
		{
			if(plotPower_>1) x[j-1] = origin[j-1]+(pmax[j-1]-origin[j-1])*pow(index/double(points_right[j-1]),plotPower_);
			else x[j-1] = pmax[j-1]+(origin[j-1]-pmax[j-1])*pow(index/double(points_right[j-1]),plotPower_);
		}
		/*plotting points on the left of the minimum*/
		else if(index<0)
		{
			if(plotPower_>1) x[j-1] = origin[j-1]+(pmin[j-1]-origin[j-1])*pow(-index/double(points_left[j-1]),plotPower_);
			else x[j-1] = pmin[j-1]+(origin[j-1]-pmin[j-1])*pow(-index/double(points_left[j-1]),plotPower_);
		}
		else 
		{
			if(plotPower_>1) x[j-1] = origin[j-1];
			else x[j-1] = pmin[j-1];	
		}
		// std::cout<<x[j-1];
		// if(j !=D ) std::cout<<",";
	}
	// std::cout<<"),(";

	for(int t=0; t<D; t++)
	{
		poiVals_[t] = x[t];
		poiVars_[t]->setVal(x[t]);
	}
	ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ? true : minim.minimize(verbose-1);
	if (ok) 
	{
		deltaNLL_ = nll.getVal() - nll0;
		double qN = 2*(deltaNLL_);
		double prob = ROOT::Math::chisquared_cdf_c(qN, D+nOtherFloatingPoi_);
		for(unsigned int j=0; j<specifiedNuis_.size(); j++){specifiedVals_[j]=specifiedVars_[j]->getVal(); }
		Combine::commitPoint(true,prob);
	}
}




    /*Begin plotting*/
    
//    std::vector<double> x(D);
//    for(int i=0; i<pow(points,D); i++){
//      for(int j=1; j<=D; j++){
//
//         for(int t=0; t<D; t++){
//	 std::cout<< index_ranges[j-1][t]<<" ";
//	 }
//	 std::cout<<std::endl;
//
//       /*plotting points on the right of the minimum*/
//	 if(index_ranges[j-1][(i/int(pow(points,j-1))%points)]>0){
//         	if(plotPower_>1) x[j-1] = origin[j-1]+(pmax[j-1]-origin[j-1])*pow(index_ranges[j-1][(i/int(pow(points,j-1))%points)]/double(points_right[j-1]),plotPower_); 
//         	else x[j-1] = pmax[j-1]+(origin[j-1]-pmax[j-1])*pow(index_ranges[j-1][(i/int(pow(points,j-1))%points)]/double(points_right[j-1]),plotPower_);
//		std::cout<<"R("<<j<<") "<<x[j-1]<<" index: "<<(i/int(pow(points,j-1))%points)<<std::endl;
//	 } 
//         
//        
//       /*plotting points on the left of the minimum*/
//	 else if(index_ranges[j-1][(i/int(pow(points,j-1))%points)]<0){ 
//         	if(plotPower_>1) x[j-1] = origin[j-1]+(pmin[j-1]-origin[j-1])*pow(-index_ranges[j-1][(i/int(pow(points,j-1))%points)]/double(points_left[j-1]),plotPower_);
//         	else  x[j-1] = pmin[j-1]+(origin[j-1]-pmin[j-1])*pow(-index_ranges[j-1][(i/int(pow(points,j-1))%points)]/double(points_left[j-1]),plotPower_); 
//		std::cout<<"L("<<j<<") "<<x[j-1]<<" index: "<<(i/int(pow(points,j-1))%points)<<std::endl;
//
//	 }
//	 
//	 else {
//	 	std::cout<<"M("<<j<<") "<<x[j-1]<<" index: "<<(i/int(pow(points,j-1))%points)<<std::endl;
//
//	 	continue;
//	      }
//       }
//       
//       for(int t=0; t<D; t++){
//         poiVals_[t] = x[t];
//         poiVars_[t]->setVal(x[t]);
//	 std::cout<< x[t]<<" ";
//       }
//       std::cout<<"\n--------------"<<std::endl;
//       
//       ok = fastScan_ || (hasMaxDeltaNLLForProf_ && (nll.getVal() - nll0) > maxDeltaNLLForProf_) ? true : minim.minimize(verbose-1);
//       if (ok) {
//           deltaNLL_ = nll.getVal() - nll0;
//           double qN = 2*(deltaNLL_);
//           double prob = ROOT::Math::chisquared_cdf_c(qN, D+nOtherFloatingPoi_);
//           for(unsigned int j=0; j<specifiedNuis_.size(); j++){specifiedVals_[j]=specifiedVars_[j]->getVal(); }
//           Combine::commitPoint(true,prob);
//       }
//        
//    }
//        


}
