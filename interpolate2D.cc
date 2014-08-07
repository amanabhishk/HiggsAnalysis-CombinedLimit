{
        TFile *_file0 = TFile::Open("2D_400points.root");  // input file from combine
        gSystem->Load("libHiggsAnalysisCombinedLimit.so");

        TTree *tree = _file0->Get("limit");

        double xmin = -2;
        double xmax = 5;
        double ymin = -1;
        double ymax = 3;

        RooRealVar rv("RV","RV",xmin,xmax);
        RooRealVar rf("RF","RF",ymin,ymax);

        RooSplineND *spline = new RooSplineND("spline","spline",RooArgList(rv,rf),tree,"deltaNLL",0.4,"2*deltaNLL<200 && quantileExpected!=-1 && quantileExpected!=1"); // arguments are ... name, title, list of parameters, the TTree from combine, the name of the branch to interpolate (i.e f(x)), a tunable parameter for the "size" of the basis functions, cut string.


        // make a 2D hist
        TH2F *hist2d = new TH2F("hist","hist",300,xmin,xmax,300,ymin,ymax);
        for ( int i = 0;i<300;i++){
          for (int j =0 ; j < 300 ; j ++){
                double rvval = hist2d.GetXaxis().GetBinCenter(i+1);
                double rfval = hist2d.GetYaxis().GetBinCenter(j+1);
                rv.setVal(rvval);
                rf.setVal(rfval);
                hist2d.SetBinContent(i+1,j+1,2*spline->getVal());
          }
        }

        hist2d->SetContour(1000);
        hist2d->SetMaximum(10);
        hist2d->SetMinimum(0);
        hist2d->Draw("colz");
        TH2F * h68 = (TH2F*)hist2d->Clone();
        h68->SetLineColor(1);
        h68->SetLineWidth(2);
        h68->SetContour(2);
        h68->SetContourLevel(1,2.3);
        h68->Draw("CONT3same");

        TH2F * h95 = (TH2F*)hist2d->Clone();
        h95->SetLineColor(1);
        h95->SetLineWidth(2);
        h95->SetLineStyle(2);
        h95->SetContour(2);
        h95->SetContourLevel(1,6.18);
        h95->Draw("CONT3same");

        TFile *out = new TFile("RVRFScan_FineGrainHist_LowerRes.root","RECREATE");
        hist2d->SetName("h2d");
        hist2d->Write();
        out->Close();

}
