# CerberusControl Implementation Progress
- [x] Phase 1: Initialize Git and .gitignore (Completed)
- [x] Phase 2: Review all ADC handling including double buffer setup in main code
- [x] Phase 3: Review and understand EXAMPLE_DBCODE_PostExtTrg.c which contains the correct manufacturer methods to set up the ADC and save double buffered data
- [x] Phase 4: Replace all ADC functionality with learnt methods from EXAMPLE_DBCODE_PostExtTrg.c
- [x] Phase 5: Refactor and remove code debt from overall control software
- [ ] Phase 6: Review and single shot functionality and compatability with other double buffer setup
- [ ] Phase 7: Review and tag for removal ADC Diagnostic button function
- [ ] Phase 8: Review safe thread queuing, remove saving but just allow quick plotting of data from acquisition 
- [ ] Phase 9: Ensure logic of code in initalize -> run config -> initilize -> continuous or single shot acquisition -> relesae works without errors, catching fatal or memory locking errors